#include "column_store.h"
#include "log_iface.h"
#include "status_util.h"
#include "query_processing/queryparser.h"

#include <boost/property_tree/ptree.hpp>

namespace Akumuli {
namespace StorageEngine {

using namespace QP;

static std::string to_string(ReshapeRequest const& req) {
    std::stringstream str;
    str << "ReshapeRequest(";
    switch (req.order_by) {
    case OrderBy::SERIES:
        str << "order-by: series, ";
        break;
    case OrderBy::TIME:
        str << "order-by: time, ";
        break;
    };
    if (req.group_by.enabled) {
        str << "group-by: enabled, ";
    } else {
        str << "group-by: disabled, ";
    }
    str << "range-begin: " << req.select.begin << ", range-end: " << req.select.end << ", ";
    str << "select: " << req.select.ids.size() << ")";
    return str.str();
}

/** This interface is used by column-store internally.
  * It allows to iterate through a bunch of columns row by row.
  */
struct RowIterator {

    virtual ~RowIterator() = default;
    /** Read samples in batch.
      * @param dest is an array that will receive values from cursor
      * @param size is an arrays size
      */
    virtual std::tuple<aku_Status, size_t> read(aku_Sample *dest, size_t size) = 0;
};


class ChainIterator : public RowIterator {
    std::vector<std::unique_ptr<NBTreeIterator>> iters_;
    std::vector<aku_ParamId> ids_;
    size_t pos_;
public:
    ChainIterator(std::vector<aku_ParamId>&& ids, std::vector<std::unique_ptr<NBTreeIterator>>&& it);
    virtual std::tuple<aku_Status, size_t> read(aku_Sample *dest, size_t size);
};


ChainIterator::ChainIterator(std::vector<aku_ParamId>&& ids, std::vector<std::unique_ptr<NBTreeIterator>>&& it)
    : iters_(std::move(it))
    , ids_(std::move(ids))
    , pos_(0)
{
}

std::tuple<aku_Status, size_t> ChainIterator::read(aku_Sample *dest, size_t size) {
    aku_Status status = AKU_ENO_DATA;
    size_t ressz = 0;  // current size
    size_t accsz = 0;  // accumulated size
    std::vector<aku_Timestamp> destts_vec(size, 0);
    std::vector<double> destval_vec(size, 0);
    std::vector<aku_ParamId> outids(size, 0);
    aku_Timestamp* destts = destts_vec.data();
    double* destval = destval_vec.data();
    while(pos_ < iters_.size()) {
        aku_ParamId curr = ids_[pos_];
        std::tie(status, ressz) = iters_[pos_]->read(destts, destval, size);
        for (size_t i = accsz; i < accsz+ressz; i++) {
            outids[i] = curr;
        }
        destts += ressz;
        destval += ressz;
        size -= ressz;
        accsz += ressz;
        if (size == 0) {
            break;
        }
        pos_++;
        if (status == AKU_ENO_DATA) {
            // this iterator is done, continue with next
            continue;
        }
        if (status != AKU_SUCCESS) {
            // Stop iteration on error!
            break;
        }
    }
    // Convert vectors to series of samples
    for (size_t i = 0; i < accsz; i++) {
        dest[i].payload.type = AKU_PAYLOAD_FLOAT;
        dest[i].paramid = outids[i];
        dest[i].timestamp = destts_vec[i];
        dest[i].payload.float64 = destval_vec[i];
    }
    return std::tie(status, accsz);
}

// ////////////// //
//  Column-store  //
// ////////////// //

ColumnStore::ColumnStore(std::shared_ptr<BlockStore> bstore)
    : blockstore_(bstore)
{
}

std::map<aku_ParamId, std::vector<StorageEngine::LogicAddr>> ColumnStore::close() {
    std::map<aku_ParamId, std::vector<StorageEngine::LogicAddr>> result;
    std::lock_guard<std::mutex> tl(table_lock_);
    Logger::msg(AKU_LOG_INFO, "Column-store commit called");
    for (auto it: columns_) {
        auto addrlist = it.second->close();
        result[it.first] = addrlist;
    }
    Logger::msg(AKU_LOG_INFO, "Column-store commit completed");
    return result;
}

aku_Status ColumnStore::create_new_column(aku_ParamId id) {
    std::vector<LogicAddr> empty;
    auto tree = std::make_shared<NBTreeExtentsList>(id, empty, blockstore_);
    {
        std::lock_guard<std::mutex> tl(table_lock_);
        if (columns_.count(id)) {
            return AKU_EBAD_ARG;
        } else {
            columns_[id] = std::move(tree);
        }
    }
    columns_[id]->force_init();
    return AKU_SUCCESS;
}


void ColumnStore::query(const ReshapeRequest &req, QP::IQueryProcessor& qproc) {
    Logger::msg(AKU_LOG_TRACE, "ColumnStore query: " + to_string(req));
    std::vector<std::unique_ptr<NBTreeIterator>> iters;
    for (auto id: req.select.ids) {
        std::lock_guard<std::mutex> lg(table_lock_); AKU_UNUSED(lg);
        auto it = columns_.find(id);
        if (it != columns_.end()) {
            std::unique_ptr<NBTreeIterator> iter = it->second->search(req.select.begin, req.select.end);
            iters.push_back(std::move(iter));
        } else {
            qproc.set_error(AKU_ENOT_FOUND);
        }
    }

    std::unique_ptr<RowIterator> iter;
    if (req.order_by == OrderBy::SERIES) {
        auto ids = req.select.ids;
        iter.reset(new ChainIterator(std::move(ids), std::move(iters)));
    } else {
        Logger::msg(AKU_LOG_ERROR, "Order-by-time not implemented yet");
        qproc.set_error(AKU_ENOT_IMPLEMENTED);
    }

    const size_t dest_size = 0x1000;
    std::vector<aku_Sample> dest;
    dest.resize(dest_size);
    aku_Status status = AKU_SUCCESS;
    while(status == AKU_SUCCESS) {
        size_t size;
        std::tie(status, size) = iter->read(dest.data(), dest_size);
        if (status != AKU_SUCCESS && status != AKU_ENO_DATA) {
            Logger::msg(AKU_LOG_ERROR, "Iteration error " + StatusUtil::str(status));
            qproc.set_error(status);
            return;
        }
        if (req.group_by.enabled) {
            for (size_t ix = 0; ix < size; ix++) {
                auto it = req.group_by.transient_map.find(dest[ix].paramid);
                if (it == req.group_by.transient_map.end()) {
                    Logger::msg(AKU_LOG_ERROR, "Unexpected id " + std::to_string(dest[ix].paramid));
                    qproc.set_error(AKU_EBAD_DATA);
                    return;
                }
            }
        } else {
            for (size_t ix = 0; ix < size; ix++) {
                if (!qproc.put(dest[ix])) {
                    return;
                }
            }
        }
    }
}

size_t ColumnStore::_get_uncommitted_memory() const {
    std::lock_guard<std::mutex> guard(table_lock_);
    size_t total_size = 0;
    for (auto const& p: columns_) {
        total_size += p.second->_get_uncommitted_size();
    }
    return total_size;
}

NBTreeAppendResult ColumnStore::write(aku_Sample const& sample, std::vector<LogicAddr>* rescue_points,
                               std::unordered_map<aku_ParamId, std::shared_ptr<NBTreeExtentsList>>* cache_or_null)
{
    std::lock_guard<std::mutex> lock(table_lock_);
    aku_ParamId id = sample.paramid;
    auto it = columns_.find(id);
    if (it != columns_.end()) {
        auto tree = it->second;
        auto res = tree->append(sample.timestamp, sample.payload.float64);
        if (res == NBTreeAppendResult::OK_FLUSH_NEEDED) {
            auto tmp = tree->get_roots();
            rescue_points->swap(tmp);
        }
        if (cache_or_null != nullptr) {
            cache_or_null->insert(std::make_pair(id, tree));
        }
        return res;
    }
    return NBTreeAppendResult::FAIL_BAD_ID;
}


// ////////////////////// //
//      WriteSession      //
// ////////////////////// //

CStoreSession::CStoreSession(std::shared_ptr<ColumnStore> registry)
    : cstore_(registry)
{
}

NBTreeAppendResult CStoreSession::write(aku_Sample const& sample, std::vector<LogicAddr> *rescue_points) {
    if (AKU_UNLIKELY(sample.payload.type != AKU_PAYLOAD_FLOAT)) {
        return NBTreeAppendResult::FAIL_BAD_VALUE;
    }
    // Cache lookup
    auto it = cache_.find(sample.paramid);
    if (it != cache_.end()) {
        return it->second->append(sample.timestamp, sample.payload.float64);
    }
    // Cache miss - access global registry
    return cstore_->write(sample, rescue_points, &cache_);
}

void CStoreSession::query(const ReshapeRequest &req, QP::IQueryProcessor& proc) {
    cstore_->query(req, proc);
}

}}  // namespace