#pragma once

// Phase 4D — EngineHost: thread-safe wrapper around MatchingEngine<Book>
// that implements IEventSink → JSON broadcast, snapshot, and message handling.
// One template instance drives either OrderBook or FastOrderBook.

#include <atomic>
#include <mutex>
#include <string>

#include "core/book_backend.hpp"
#include "core/event_sink.hpp"
#include "core/matching_engine.hpp"
#include "core/order.hpp"
#include "gateway/json.hpp"
#include "gateway/server.hpp"

namespace gateway {

template <lob::BookBackend Book>
class EngineHost : public lob::IEventSink {
public:
    EngineHost(Book& book, Server& server)
        : book_(book), server_(server), engine_(book, this) {}

    // Called by Server for each incoming JSON payload (length-prefix or WS text).
    // Returns a JSON string to send directly back to that session (error case),
    // or empty if the message was handled via broadcast (normal order flow).
    // Thread-safe: acquires mtx_ for book/engine access.
    std::string handleMessage(const std::string& jsonStr, int sessionId);

    // IEventSink — called from engine.processOrder while mtx_ is held.
    // Each event is serialized to JSON and broadcast to all sessions.
    void onTrade(const lob::Trade& t) override;
    void onOrderUpdate(const lob::ExecutionReport& r) override;
    void onBookTick(const lob::BookTick& tk) override;

    // Send snapshot of current book to one session (on connect / subscribe)
    void sendSnapshot(int sessionId);

private:
    // Helpers to build JSON objects (each assigns nextBcastSeq_++)
    JsonValue makeTradeJson(const lob::Trade& t);
    JsonValue makeReportJson(const lob::ExecutionReport& r);
    JsonValue makeTickJson(const lob::BookTick& tk);
    JsonValue makeSnapshotJson();
    JsonValue makeErrorJson(const std::string& reason);

    Book& book_;
    Server& server_;
    lob::MatchingEngine<Book> engine_;
    std::mutex mtx_;
    std::atomic<uint64_t> nextBcastSeq_{1};
};

// ---------------------------------------------------------------------------
// Implementation — header-inline (template)
// ---------------------------------------------------------------------------

template <lob::BookBackend Book>
JsonValue EngineHost<Book>::makeTradeJson(const lob::Trade& t) {
    JsonValue o = JsonValue::makeObject();
    o.set("type", JsonValue("trade"));
    o.set("seq", JsonValue(static_cast<int64_t>(nextBcastSeq_++)));
    o.set("tradeId", JsonValue(static_cast<int64_t>(t.tradeId)));
    o.set("takerId", JsonValue(static_cast<int64_t>(t.takerId)));
    o.set("makerId", JsonValue(static_cast<int64_t>(t.makerId)));
    o.set("side", JsonValue(t.takerSide == lob::Side::Buy ? "buy" : "sell"));
    o.set("price", JsonValue(t.price));
    o.set("qty", JsonValue(static_cast<int64_t>(t.qty)));
    o.set("ts", JsonValue(static_cast<int64_t>(t.ts)));
    return o;
}

template <lob::BookBackend Book>
JsonValue EngineHost<Book>::makeReportJson(const lob::ExecutionReport& r) {
    JsonValue o = JsonValue::makeObject();
    o.set("type", JsonValue("execution.report"));
    o.set("seq", JsonValue(static_cast<int64_t>(nextBcastSeq_++)));
    o.set("orderId", JsonValue(static_cast<int64_t>(r.orderId)));
    o.set("side", JsonValue(r.side == lob::Side::Buy ? "buy" : "sell"));
    o.set("price", JsonValue(r.price));
    o.set("qty", JsonValue(static_cast<int64_t>(r.qty)));
    o.set("filled", JsonValue(static_cast<int64_t>(r.filled)));
    o.set("remaining", JsonValue(static_cast<int64_t>(r.remaining)));
    o.set("status", JsonValue(lob::toString(r.status)));
    o.set("orderType", JsonValue(lob::toString(r.type)));
    o.set("seqOrder", JsonValue(static_cast<int64_t>(r.seq)));
    if (!r.reason.empty()) o.set("reason", JsonValue(r.reason));
    return o;
}

template <lob::BookBackend Book>
JsonValue EngineHost<Book>::makeTickJson(const lob::BookTick& tk) {
    JsonValue o = JsonValue::makeObject();
    o.set("type", JsonValue("marketdata.tick"));
    o.set("seq", JsonValue(static_cast<int64_t>(nextBcastSeq_++)));
    o.set("side", JsonValue(tk.side == lob::Side::Buy ? "buy" : "sell"));
    o.set("price", JsonValue(tk.price));
    o.set("qty", JsonValue(static_cast<int64_t>(tk.totalQuantity)));
    o.set("removed", JsonValue(tk.removed));
    o.set("isBest", JsonValue(tk.isBest));
    return o;
}

template <lob::BookBackend Book>
JsonValue EngineHost<Book>::makeSnapshotJson() {
    JsonValue o = JsonValue::makeObject();
    o.set("type", JsonValue("marketdata.snapshot"));
    o.set("seq", JsonValue(static_cast<int64_t>(nextBcastSeq_++)));
    // bids/asks arrays
    JsonValue bids = JsonValue::makeArray();
    JsonValue asks = JsonValue::makeArray();
    if constexpr (std::is_same_v<Book, lob::OrderBook>) {
        // OrderBook: iterate map
        for (auto it = book_.bids().rbegin(); it != book_.bids().rend(); ++it) {
            JsonValue lvl = JsonValue::makeObject();
            lvl.set("price", JsonValue(it->first));
            lvl.set("qty", JsonValue(static_cast<int64_t>(it->second.totalQuantity())));
            bids.push_back(std::move(lvl));
        }
        for (auto it = book_.asks().begin(); it != book_.asks().end(); ++it) {
            JsonValue lvl = JsonValue::makeObject();
            lvl.set("price", JsonValue(it->first));
            lvl.set("qty", JsonValue(static_cast<int64_t>(it->second.totalQuantity())));
            asks.push_back(std::move(lvl));
        }
    } else {
        // FastOrderBook: forEachLevel already best-first
        book_.forEachLevel(lob::Side::Buy, [&](const lob::FastPriceLevel* lvl){
            JsonValue j = JsonValue::makeObject();
            j.set("price", JsonValue(lvl->price));
            j.set("qty", JsonValue(static_cast<int64_t>(lvl->totalQuantity())));
            bids.push_back(std::move(j));
        });
        book_.forEachLevel(lob::Side::Sell, [&](const lob::FastPriceLevel* lvl){
            JsonValue j = JsonValue::makeObject();
            j.set("price", JsonValue(lvl->price));
            j.set("qty", JsonValue(static_cast<int64_t>(lvl->totalQuantity())));
            asks.push_back(std::move(j));
        });
    }
    o.set("bids", std::move(bids));
    o.set("asks", std::move(asks));
    auto bb = book_.bestBid();
    auto ba = book_.bestAsk();
    if (bb.has_value()) o.set("bestBid", JsonValue(*bb));
    if (ba.has_value()) o.set("bestAsk", JsonValue(*ba));
    return o;
}

template <lob::BookBackend Book>
JsonValue EngineHost<Book>::makeErrorJson(const std::string& reason) {
    JsonValue o = JsonValue::makeObject();
    o.set("type", JsonValue("error"));
    o.set("seq", JsonValue(static_cast<int64_t>(nextBcastSeq_++)));
    o.set("reason", JsonValue(reason));
    return o;
}

template <lob::BookBackend Book>
void EngineHost<Book>::onTrade(const lob::Trade& t) {
    auto j = makeTradeJson(t);
    server_.broadcast(j.stringify());
}

template <lob::BookBackend Book>
void EngineHost<Book>::onOrderUpdate(const lob::ExecutionReport& r) {
    auto j = makeReportJson(r);
    server_.broadcast(j.stringify());
}

template <lob::BookBackend Book>
void EngineHost<Book>::onBookTick(const lob::BookTick& tk) {
    auto j = makeTickJson(tk);
    server_.broadcast(j.stringify());
}

template <lob::BookBackend Book>
void EngineHost<Book>::sendSnapshot(int sessionId) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto j = makeSnapshotJson();
    server_.sendTo(sessionId, j.stringify());
}

template <lob::BookBackend Book>
std::string EngineHost<Book>::handleMessage(const std::string& jsonStr, int sessionId) {
    auto parsed = JsonValue::parse(jsonStr);
    if (!parsed) {
        auto err = makeErrorJson("invalid_json");
        return err.stringify(); // direct reply
    }
    const JsonValue& v = *parsed;
    if (!v.isObject()) {
        auto err = makeErrorJson("expected_object");
        return err.stringify();
    }
    const JsonValue* tp = v.get("type");
    if (!tp || !tp->isString()) {
        auto err = makeErrorJson("missing_type");
        return err.stringify();
    }
    std::string type = tp->asString();
    if (type == "order.new") {
        const JsonValue* idv = v.get("id");
        const JsonValue* sidev = v.get("side");
        const JsonValue* pricev = v.get("price");
        const JsonValue* qtyv = v.get("qty");
        if (!idv || !sidev || !qtyv) {
            auto err = makeErrorJson("missing_fields_order.new");
            return err.stringify();
        }
        lob::Order o;
        o.id = static_cast<lob::OrderId>(idv->isInt() ? idv->asInt() : static_cast<int64_t>(idv->asDouble()));
        std::string sside = sidev->asString();
        o.side = (sside == "buy" ? lob::Side::Buy : lob::Side::Sell);
        o.price = pricev ? (pricev->isInt() ? pricev->asInt() : static_cast<int64_t>(pricev->asDouble())) : 0;
        o.qty = static_cast<lob::Quantity>(qtyv->isInt() ? qtyv->asInt() : static_cast<int64_t>(qtyv->asDouble()));
        o.remaining = o.qty;
        const JsonValue* otv = v.get("orderType");
        if (otv && otv->isString()) {
            std::string ot = otv->asString();
            if (ot == "market") o.type = lob::OrderType::Market;
            else if (ot == "ioc") o.type = lob::OrderType::IOC;
            else if (ot == "fok") o.type = lob::OrderType::FOK;
            else o.type = lob::OrderType::Limit;
        } else {
            o.type = lob::OrderType::Limit;
        }
        const JsonValue* tsv = v.get("ts");
        if (tsv && tsv->isInt()) o.ts = static_cast<lob::Timestamp>(tsv->asInt());
        // per-order validation at boundary is done by engine; we just call
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // engine will emit via onTrade/onOrderUpdate/onBookTick -> broadcast
            (void)engine_.processOrder(o);
        }
        return ""; // broadcasts already sent
    } else if (type == "order.cancel") {
        const JsonValue* idv = v.get("id");
        if (!idv) {
            auto err = makeErrorJson("missing_id");
            return err.stringify();
        }
        lob::OrderId id = static_cast<lob::OrderId>(idv->isInt() ? idv->asInt() : static_cast<int64_t>(idv->asDouble()));
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ok = engine_.cancelOrder(id);
        }
        if (!ok) {
            auto err = makeErrorJson("unknown_id");
            // still return error to requestor, but also broadcast? For unknown cancel, we send error directly
            return err.stringify();
        }
        return "";
    } else if (type == "order.replace") {
        const JsonValue* idv = v.get("id");
        const JsonValue* pricev = v.get("price");
        const JsonValue* qtyv = v.get("qty");
        if (!idv || !pricev || !qtyv) {
            auto err = makeErrorJson("missing_fields_order.replace");
            return err.stringify();
        }
        lob::OrderId id = static_cast<lob::OrderId>(idv->isInt() ? idv->asInt() : static_cast<int64_t>(idv->asDouble()));
        lob::Price price = pricev->isInt() ? pricev->asInt() : static_cast<lob::Price>(pricev->asDouble());
        lob::Quantity qty = static_cast<lob::Quantity>(qtyv->isInt() ? qtyv->asInt() : static_cast<int64_t>(qtyv->asDouble()));
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ok = engine_.modifyOrder(id, price, qty);
        }
        if (!ok) {
            auto err = makeErrorJson("unknown_id_or_rejected");
            return err.stringify();
        }
        return "";
    } else if (type == "subscribe" || type == "marketdata.subscribe") {
        sendSnapshot(sessionId);
        return "";
    } else if (type == "ping") {
        JsonValue pong = JsonValue::makeObject();
        pong.set("type", JsonValue("pong"));
        pong.set("seq", JsonValue(static_cast<int64_t>(nextBcastSeq_++)));
        const JsonValue* idv = v.get("id");
        if (idv) pong.set("id", *idv);
        return pong.stringify();
    } else {
        auto err = makeErrorJson("unknown_type");
        return err.stringify();
    }
}

} // namespace gateway
