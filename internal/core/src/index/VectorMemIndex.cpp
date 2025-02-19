// Licensed to the LF AI & Data foundation under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "index/VectorMemIndex.h"

#include <cmath>
#include "index/Meta.h"
#include "index/Utils.h"
#include "exceptions/EasyAssert.h"
#include "config/ConfigKnowhere.h"

#include "knowhere/factory.h"
#include "knowhere/comp/time_recorder.h"
#include "common/BitsetView.h"
#include "common/Slice.h"
#include "common/Consts.h"
#include "common/RangeSearchHelper.h"
#include "common/Utils.h"

namespace milvus::index {

VectorMemIndex::VectorMemIndex(const IndexType& index_type,
                               const MetricType& metric_type,
                               storage::FileManagerImplPtr file_manager)
    : VectorIndex(index_type, metric_type) {
    AssertInfo(!is_unsupported(index_type, metric_type),
               index_type + " doesn't support metric: " + metric_type);
    if (file_manager != nullptr) {
        file_manager_ = std::dynamic_pointer_cast<storage::MemFileManagerImpl>(
            file_manager);
    }
    index_ = knowhere::IndexFactory::Instance().Create(GetIndexType());
}

BinarySet
VectorMemIndex::Upload(const Config& config) {
    auto binary_set = Serialize(config);
    file_manager_->AddFile(binary_set);

    auto remote_paths_to_size = file_manager_->GetRemotePathsToFileSize();
    BinarySet ret;
    for (auto& file : remote_paths_to_size) {
        ret.Append(file.first, nullptr, file.second);
    }

    return ret;
}

BinarySet
VectorMemIndex::Serialize(const Config& config) {
    knowhere::BinarySet ret;
    auto stat = index_.Serialize(ret);
    if (stat != knowhere::Status::success)
        PanicCodeInfo(ErrorCodeEnum::UnexpectedError,
                      "failed to serialize index, " + MatchKnowhereError(stat));
    Disassemble(ret);

    return ret;
}

void
VectorMemIndex::LoadWithoutAssemble(const BinarySet& binary_set,
                                    const Config& config) {
    auto stat = index_.Deserialize(binary_set);
    if (stat != knowhere::Status::success)
        PanicCodeInfo(
            ErrorCodeEnum::UnexpectedError,
            "failed to Deserialize index, " + MatchKnowhereError(stat));
    SetDim(index_.Dim());
}

void
VectorMemIndex::Load(const BinarySet& binary_set, const Config& config) {
    milvus::Assemble(const_cast<BinarySet&>(binary_set));
    LoadWithoutAssemble(binary_set, config);
}

void
VectorMemIndex::Load(const Config& config) {
    auto index_files =
        GetValueFromConfig<std::vector<std::string>>(config, "index_files");
    AssertInfo(index_files.has_value(),
               "index file paths is empty when load index");
    auto index_datas = file_manager_->LoadIndexToMemory(index_files.value());
    AssembleIndexDatas(index_datas);
    BinarySet binary_set;
    for (auto& [key, data] : index_datas) {
        auto size = data->Size();
        auto deleter = [&](uint8_t*) {};  // avoid repeated deconstruction
        auto buf = std::shared_ptr<uint8_t[]>(
            (uint8_t*)const_cast<void*>(data->Data()), deleter);
        binary_set.Append(key, buf, size);
    }
    LoadWithoutAssemble(binary_set, config);
}

void
VectorMemIndex::BuildWithDataset(const DatasetPtr& dataset,
                                 const Config& config) {
    knowhere::Json index_config;
    index_config.update(config);

    SetDim(dataset->GetDim());

    knowhere::TimeRecorder rc("BuildWithoutIds", 1);
    auto stat = index_.Build(*dataset, index_config);
    if (stat != knowhere::Status::success)
        PanicCodeInfo(ErrorCodeEnum::BuildIndexError,
                      "failed to build index, " + MatchKnowhereError(stat));
    rc.ElapseFromBegin("Done");
    SetDim(index_.Dim());
}

void
VectorMemIndex::Build(const Config& config) {
    auto insert_files =
        GetValueFromConfig<std::vector<std::string>>(config, "insert_files");
    AssertInfo(insert_files.has_value(),
               "insert file paths is empty when build disk ann index");
    auto field_datas =
        file_manager_->CacheRawDataToMemory(insert_files.value());

    int64_t total_size = 0;
    int64_t total_num_rows = 0;
    int64_t dim = 0;
    for (auto data : field_datas) {
        total_size += data->Size();
        total_num_rows += data->get_num_rows();
        AssertInfo(dim == 0 || dim == data->get_dim(),
                   "inconsistent dim value between field datas!");
        dim = data->get_dim();
    }

    auto buf = std::shared_ptr<uint8_t[]>(new uint8_t[total_size]);
    int64_t offset = 0;
    for (auto data : field_datas) {
        std::memcpy(buf.get() + offset, data->Data(), data->Size());
        offset += data->Size();
        data.reset();
    }
    field_datas.clear();

    Config build_config;
    build_config.update(config);
    build_config.erase("insert_files");

    auto dataset = GenDataset(total_num_rows, dim, buf.get());
    BuildWithDataset(dataset, build_config);
}

void
VectorMemIndex::AddWithDataset(const DatasetPtr& dataset,
                               const Config& config) {
    knowhere::Json index_config;
    index_config.update(config);

    knowhere::TimeRecorder rc("AddWithDataset", 1);
    auto stat = index_.Add(*dataset, index_config);
    if (stat != knowhere::Status::success)
        PanicCodeInfo(ErrorCodeEnum::BuildIndexError,
                      "failed to append index, " + MatchKnowhereError(stat));
    rc.ElapseFromBegin("Done");
}

std::unique_ptr<SearchResult>
VectorMemIndex::Query(const DatasetPtr dataset,
                      const SearchInfo& search_info,
                      const BitsetView& bitset) {
    //    AssertInfo(GetMetricType() == search_info.metric_type_,
    //               "Metric type of field index isn't the same with search info");

    auto num_queries = dataset->GetRows();
    knowhere::Json search_conf = search_info.search_params_;
    auto topk = search_info.topk_;
    // TODO :: check dim of search data
    auto final = [&] {
        search_conf[knowhere::meta::TOPK] = topk;
        search_conf[knowhere::meta::METRIC_TYPE] = GetMetricType();
        auto index_type = GetIndexType();
        if (CheckKeyInConfig(search_conf, RADIUS)) {
            if (CheckKeyInConfig(search_conf, RANGE_FILTER)) {
                CheckRangeSearchParam(search_conf[RADIUS],
                                      search_conf[RANGE_FILTER],
                                      GetMetricType());
            }
            auto res = index_.RangeSearch(*dataset, search_conf, bitset);
            if (!res.has_value()) {
                PanicCodeInfo(ErrorCodeEnum::UnexpectedError,
                              "failed to range search, " +
                                  MatchKnowhereError(res.error()));
            }
            return ReGenRangeSearchResult(
                res.value(), topk, num_queries, GetMetricType());
        } else {
            auto res = index_.Search(*dataset, search_conf, bitset);
            if (!res.has_value()) {
                PanicCodeInfo(
                    ErrorCodeEnum::UnexpectedError,
                    "failed to search, " + MatchKnowhereError(res.error()));
            }
            return res.value();
        }
    }();

    auto ids = final->GetIds();
    float* distances = const_cast<float*>(final->GetDistance());
    final->SetIsOwner(true);
    auto round_decimal = search_info.round_decimal_;
    auto total_num = num_queries * topk;

    if (round_decimal != -1) {
        const float multiplier = pow(10.0, round_decimal);
        for (int i = 0; i < total_num; i++) {
            distances[i] = std::round(distances[i] * multiplier) / multiplier;
        }
    }
    auto result = std::make_unique<SearchResult>();
    result->seg_offsets_.resize(total_num);
    result->distances_.resize(total_num);
    result->total_nq_ = num_queries;
    result->unity_topK_ = topk;

    std::copy_n(ids, total_num, result->seg_offsets_.data());
    std::copy_n(distances, total_num, result->distances_.data());

    return result;
}

const bool
VectorMemIndex::HasRawData() const {
    return index_.HasRawData(GetMetricType());
}

std::vector<uint8_t>
VectorMemIndex::GetVector(const DatasetPtr dataset) const {
    auto res = index_.GetVectorByIds(*dataset);
    if (!res.has_value()) {
        PanicCodeInfo(
            ErrorCodeEnum::UnexpectedError,
            "failed to get vector, " + MatchKnowhereError(res.error()));
    }
    auto index_type = GetIndexType();
    auto tensor = res.value()->GetTensor();
    auto row_num = res.value()->GetRows();
    auto dim = res.value()->GetDim();
    int64_t data_size;
    if (is_in_bin_list(index_type)) {
        data_size = dim / 8 * row_num;
    } else {
        data_size = dim * row_num * sizeof(float);
    }
    std::vector<uint8_t> raw_data;
    raw_data.resize(data_size);
    memcpy(raw_data.data(), tensor, data_size);
    return raw_data;
}

}  // namespace milvus::index
