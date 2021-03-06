// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "db/insert/MemCollection.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <utility>

#include <fiu/fiu-local.h>

#include "config/ServerConfig.h"
#include "db/SnapshotUtils.h"
#include "db/Utils.h"
#include "db/snapshot/CompoundOperations.h"
#include "db/snapshot/IterateHandler.h"
#include "db/snapshot/Snapshots.h"
#include "db/wal/WalManager.h"
#include "utils/CommonUtil.h"
#include "utils/Log.h"
#include "utils/TimeRecorder.h"

namespace milvus {
namespace engine {

MemCollection::MemCollection(int64_t collection_id, const DBOptions& options)
    : collection_id_(collection_id), options_(options) {
    GetSegmentRowCount(collection_id_, segment_row_count_);
}

Status
MemCollection::Add(int64_t partition_id, const DataChunkPtr& chunk, idx_t op_id) {
    std::lock_guard<std::mutex> lock(mem_mutex_);
    MemSegmentPtr current_mem_segment;
    auto pair = mem_segments_.find(partition_id);
    if (pair != mem_segments_.end()) {
        MemSegmentList& segments = pair->second;
        if (!segments.empty()) {
            current_mem_segment = segments.back();
        }
    }

    int64_t chunk_size = utils::GetSizeOfChunk(chunk);

    Status status;
    if (current_mem_segment == nullptr || chunk->count_ >= segment_row_count_ ||
        current_mem_segment->GetCurrentRowCount() >= segment_row_count_ ||
        current_mem_segment->GetCurrentMem() + chunk_size > MAX_MEM_SEGMENT_SIZE) {
        MemSegmentPtr new_mem_segment = std::make_shared<MemSegment>(collection_id_, partition_id, options_);
        status = new_mem_segment->Add(chunk, op_id);
        if (status.ok()) {
            mem_segments_[partition_id].emplace_back(new_mem_segment);
        } else {
            return status;
        }
    } else {
        status = current_mem_segment->Add(chunk, op_id);
    }

    if (!status.ok()) {
        std::string err_msg = "Insert failed: " + status.ToString();
        LOG_ENGINE_ERROR_ << LogOut("[%s][%ld] ", "insert", 0) << err_msg;
        return Status(DB_ERROR, err_msg);
    }

    return Status::OK();
}

Status
MemCollection::Delete(const std::vector<idx_t>& ids, idx_t op_id) {
    if (ids.empty()) {
        return Status::OK();
    }

    // Add the id so it can be applied to segment files during the next flush
    for (auto& id : ids) {
        ids_to_delete_.insert(id);
    }

    // Add the id to mem segments so it can be applied during the next flush
    std::lock_guard<std::mutex> lock(mem_mutex_);
    for (auto& partition_segments : mem_segments_) {
        for (auto& segment : partition_segments.second) {
            segment->Delete(ids, op_id);
        }
    }

    return Status::OK();
}

Status
MemCollection::EraseMem(int64_t partition_id) {
    std::lock_guard<std::mutex> lock(mem_mutex_);
    auto pair = mem_segments_.find(partition_id);
    if (pair != mem_segments_.end()) {
        mem_segments_.erase(pair);
    }

    return Status::OK();
}

Status
MemCollection::Serialize() {
    TimeRecorder recorder("MemCollection::Serialize collection " + std::to_string(collection_id_));

    // Operation ApplyDelete need retry if ss stale error found
    while (true) {
        auto status = ApplyDeleteToFile();
        if (status.ok()) {
            ids_to_delete_.clear();
            break;
        } else if (status.code() == SS_STALE_ERROR) {
            LOG_ENGINE_ERROR_ << "Failed to apply deleted ids to segment files: file stale. Try again";
            continue;
        } else {
            LOG_ENGINE_ERROR_ << "Failed to apply deleted ids to segment files: " << status.ToString();
            // Note: don't return here, continue serialize mem segments
            break;
        }
    }

    // serialize mem to new segment files
    // delete ids will be applied in MemSegment::Serialize() method
    std::lock_guard<std::mutex> lock(mem_mutex_);
    for (auto& partition_segments : mem_segments_) {
        MemSegmentList& segments = partition_segments.second;
        for (auto& segment : segments) {
            auto status = segment->Serialize();
            if (!status.ok()) {
                return status;
            }
        }
    }

    mem_segments_.clear();

    recorder.RecordSection("Finished flushing");

    return Status::OK();
}

Status
MemCollection::ApplyDeleteToFile() {
    if (ids_to_delete_.empty()) {
        return Status::OK();
    }

    // iterate each segment to delete entities
    snapshot::ScopedSnapshotT ss;
    STATUS_CHECK(snapshot::Snapshots::GetInstance().GetSnapshot(ss, collection_id_));

    snapshot::OperationContext context;
    auto segments_op = std::make_shared<snapshot::CompoundSegmentsOperation>(context, ss);

    int64_t segment_iterated = 0;
    auto segment_executor = [&](const snapshot::SegmentPtr& segment, snapshot::SegmentIterator* iterator) -> Status {
        segment_iterated++;
        auto seg_visitor = engine::SegmentVisitor::Build(ss, segment->GetID());
        segment::SegmentReaderPtr segment_reader =
            std::make_shared<segment::SegmentReader>(options_.meta_.path_, seg_visitor);

        // Step 1: Check delete_id in mem
        std::set<idx_t> ids_to_check;
        {
            segment::IdBloomFilterPtr pre_bloom_filter;
            STATUS_CHECK(segment_reader->LoadBloomFilter(pre_bloom_filter));
            for (auto& id : ids_to_delete_) {
                if (pre_bloom_filter->Check(id)) {
                    ids_to_check.insert(id);
                }
            }
        }

        if (ids_to_check.empty()) {
            return Status::OK();
        }

        std::vector<engine::idx_t> uids;
        STATUS_CHECK(segment_reader->LoadUids(uids));

        // Step 2: Mark previous deleted docs file and bloom filter file stale
        auto& field_visitors_map = seg_visitor->GetFieldVisitors();
        auto uid_field_visitor = seg_visitor->GetFieldVisitor(engine::FIELD_UID);
        auto del_doc_visitor = uid_field_visitor->GetElementVisitor(FieldElementType::FET_DELETED_DOCS);
        auto del_docs_element = del_doc_visitor->GetElement();
        auto blm_filter_visitor = uid_field_visitor->GetElementVisitor(FieldElementType::FET_BLOOM_FILTER);
        auto blm_filter_element = blm_filter_visitor->GetElement();

        auto segment_file_executor = [&](const snapshot::SegmentFilePtr& segment_file,
                                         snapshot::SegmentFileIterator* iterator) -> Status {
            if (segment_file->GetSegmentId() == segment->GetID() &&
                (segment_file->GetFieldElementId() == del_docs_element->GetID() ||
                 segment_file->GetFieldElementId() == blm_filter_element->GetID())) {
                segments_op->AddStaleSegmentFile(segment_file);
            }

            return Status::OK();
        };

        auto segment_file_iterator = std::make_shared<snapshot::SegmentFileIterator>(ss, segment_file_executor);
        segment_file_iterator->Iterate();
        STATUS_CHECK(segment_file_iterator->GetStatus());

        // Step 3: Create new deleted docs file and bloom filter file
        snapshot::SegmentFileContext del_file_context;
        del_file_context.field_name = uid_field_visitor->GetField()->GetName();
        del_file_context.field_element_name = del_docs_element->GetName();
        del_file_context.collection_id = segment->GetCollectionId();
        del_file_context.partition_id = segment->GetPartitionId();
        del_file_context.segment_id = segment->GetID();
        snapshot::SegmentFilePtr delete_file;
        STATUS_CHECK(segments_op->CommitNewSegmentFile(del_file_context, delete_file));

        std::string collection_root_path = options_.meta_.path_ + COLLECTIONS_FOLDER;
        auto segment_writer = std::make_shared<segment::SegmentWriter>(options_.meta_.path_, seg_visitor);

        std::string del_docs_path = snapshot::GetResPath<snapshot::SegmentFile>(collection_root_path, delete_file);

        snapshot::SegmentFileContext bloom_file_context;
        bloom_file_context.field_name = uid_field_visitor->GetField()->GetName();
        bloom_file_context.field_element_name = blm_filter_element->GetName();
        bloom_file_context.collection_id = segment->GetCollectionId();
        bloom_file_context.partition_id = segment->GetPartitionId();
        bloom_file_context.segment_id = segment->GetID();

        engine::snapshot::SegmentFile::Ptr bloom_filter_file;
        STATUS_CHECK(segments_op->CommitNewSegmentFile(bloom_file_context, bloom_filter_file));

        std::string bloom_filter_file_path =
            snapshot::GetResPath<snapshot::SegmentFile>(collection_root_path, bloom_filter_file);

        // Step 4: update delete docs and bloom filter
        {
            segment::IdBloomFilterPtr bloom_filter;
            STATUS_CHECK(segment_writer->CreateBloomFilter(bloom_filter_file_path, bloom_filter));
            std::vector<engine::offset_t> delete_docs_offset;
            for (size_t i = 0; i < uids.size(); i++) {
                if (std::binary_search(ids_to_check.begin(), ids_to_check.end(), uids[i])) {
                    delete_docs_offset.emplace_back(i);
                } else {
                    bloom_filter->Add(uids[i]);
                }
            }

            STATUS_CHECK(segments_op->CommitRowCountDelta(segment->GetID(), delete_docs_offset.size(), true));

            // Load previous delete_id and merge into 'delete_ids'
            segment::DeletedDocsPtr prev_del_docs;
            STATUS_CHECK(segment_reader->LoadDeletedDocs(prev_del_docs));
            if (prev_del_docs) {
                auto& pre_del_offsets = prev_del_docs->GetDeletedDocs();
                size_t delete_docs_size = delete_docs_offset.size();
                for (auto& offset : pre_del_offsets) {
                    if (!std::binary_search(delete_docs_offset.begin(), delete_docs_offset.begin() + delete_docs_size,
                                            offset)) {
                        delete_docs_offset.emplace_back(offset);
                    }
                }
            }
            std::sort(delete_docs_offset.begin(), delete_docs_offset.end());

            auto delete_docs = std::make_shared<segment::DeletedDocs>(delete_docs_offset);
            STATUS_CHECK(segment_writer->WriteDeletedDocs(del_docs_path, delete_docs));
            STATUS_CHECK(segment_writer->WriteBloomFilter(bloom_filter_file_path, bloom_filter));
        }

        delete_file->SetSize(CommonUtil::GetFileSize(del_docs_path + codec::DeletedDocsFormat::FilePostfix()));
        bloom_filter_file->SetSize(
            CommonUtil::GetFileSize(bloom_filter_file_path + codec::IdBloomFilterFormat::FilePostfix()));

        return Status::OK();
    };

    auto segment_iterator = std::make_shared<snapshot::SegmentIterator>(ss, segment_executor);
    segment_iterator->Iterate();
    STATUS_CHECK(segment_iterator->GetStatus());

    if (segment_iterated == 0) {
        return Status::OK();  // no segment, nothing to do
    }

    fiu_do_on("MemCollection.ApplyDeletes.RandomSleep", sleep(1));
    return segments_op->Push();
}

int64_t
MemCollection::GetCollectionId() const {
    return collection_id_;
}

size_t
MemCollection::GetCurrentMem() {
    std::lock_guard<std::mutex> lock(mem_mutex_);
    size_t total_mem = 0;
    for (auto& partition_segments : mem_segments_) {
        MemSegmentList& segments = partition_segments.second;
        for (auto& segment : segments) {
            total_mem += segment->GetCurrentMem();
        }
    }
    return total_mem;
}

}  // namespace engine
}  // namespace milvus
