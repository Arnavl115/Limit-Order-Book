#include "core/event_sink.hpp"
#include "core/match_types.hpp"
#include "test_framework.hpp"

using namespace lob;

TEST(match_types_trade_defaults) {
    Trade t;
    CHECK(t.tradeId == 0);
    CHECK(t.seq == 0);
    CHECK(t.takerId == 0);
    CHECK(t.makerId == 0);
    CHECK(t.price == 0);
    CHECK(t.qty == 0);
    CHECK(t.takerSide == Side::Buy);
    std::string s = toString(t);
    CHECK(s.find("Trade{") != std::string::npos);
    CHECK(s.find("taker=") != std::string::npos);
}

TEST(match_types_trade_toString_contains_fields) {
    Trade t;
    t.tradeId = 42;
    t.seq = 7;
    t.takerId = 100;
    t.makerId = 200;
    t.takerSide = Side::Sell;
    t.price = 12345;
    t.qty = 50;
    std::string s = toString(t);
    CHECK(s.find("42") != std::string::npos);
    CHECK(s.find("100") != std::string::npos);
    CHECK(s.find("200") != std::string::npos);
    CHECK(s.find("sell") != std::string::npos);
    CHECK(s.find("12345") != std::string::npos);
}

TEST(match_types_exec_status_strings) {
    CHECK(std::string(toString(ExecStatus::New)) == "new");
    CHECK(std::string(toString(ExecStatus::PartiallyFilled)) == "partially_filled");
    CHECK(std::string(toString(ExecStatus::Filled)) == "filled");
    CHECK(std::string(toString(ExecStatus::Cancelled)) == "cancelled");
    CHECK(std::string(toString(ExecStatus::Rejected)) == "rejected");
    CHECK(std::string(toString(ExecStatus::Resting)) == "resting");
}

TEST(match_types_execution_report_defaults) {
    ExecutionReport r;
    CHECK(r.orderId == 0);
    CHECK(r.qty == 0);
    CHECK(r.filled == 0);
    CHECK(r.remaining == 0);
    CHECK(r.status == ExecStatus::New);
    CHECK(r.seq == 0);
    CHECK(r.reason.empty());
    std::string s = toString(r);
    CHECK(s.find("Exec{") != std::string::npos);
}

TEST(match_types_execution_report_toString_with_reason) {
    ExecutionReport r;
    r.orderId = 1;
    r.status = ExecStatus::Rejected;
    r.reason = "duplicate_id";
    std::string s = toString(r);
    CHECK(s.find("rejected") != std::string::npos);
    CHECK(s.find("duplicate_id") != std::string::npos);
}

TEST(match_types_booktick_defaults) {
    BookTick tk;
    CHECK(tk.price == 0);
    CHECK(tk.totalQuantity == 0);
    CHECK(!tk.removed);
    CHECK(!tk.isBest);
    std::string s = toString(tk);
    CHECK(s.find("Tick{") != std::string::npos);
}

TEST(match_types_match_result_defaults) {
    MatchResult mr;
    CHECK(mr.takerId == 0);
    CHECK(mr.status == ExecStatus::Rejected);
    CHECK(mr.filled == 0);
    CHECK(mr.remaining == 0);
    CHECK(mr.trades.empty());
    CHECK(!mr.bestBidBefore.has_value());
    CHECK(!mr.bestAskAfter.has_value());
    CHECK(mr.isRejected());
    std::string s = toString(mr);
    CHECK(s.find("Match{") != std::string::npos);
}

TEST(match_types_match_result_isFilled_helper) {
    MatchResult mr;
    mr.status = ExecStatus::Filled;
    CHECK(mr.isFilled());
    CHECK(!mr.isRejected());
}

TEST(event_sink_null_does_not_crash) {
    NullEventSink sink;
    Trade t;
    t.tradeId = 1;
    sink.onTrade(t);
    ExecutionReport r;
    sink.onOrderUpdate(r);
    BookTick tk;
    sink.onBookTick(tk);
    // no CHECK needed — absence of crash is the test
    CHECK(true);
}

TEST(event_sink_counting_tracks_invocations) {
    CountingEventSink sink;
    CHECK(sink.tradeCount == 0);
    CHECK(sink.orderUpdateCount == 0);
    CHECK(sink.bookTickCount == 0);

    Trade t;
    t.tradeId = 7;
    t.takerId = 1;
    t.makerId = 2;
    t.price = 100;
    t.qty = 10;
    sink.onTrade(t);
    CHECK(sink.tradeCount == 1);
    CHECK(sink.trades.size() == 1);
    CHECK(sink.lastTrade.tradeId == 7);

    ExecutionReport r;
    r.orderId = 1;
    r.status = ExecStatus::Filled;
    sink.onOrderUpdate(r);
    CHECK(sink.orderUpdateCount == 1);
    CHECK(sink.reports.size() == 1);
    CHECK(sink.lastReport.orderId == 1);

    BookTick tk;
    tk.price = 100;
    tk.totalQuantity = 0;
    tk.removed = true;
    sink.onBookTick(tk);
    CHECK(sink.bookTickCount == 1);
    CHECK(sink.ticks.size() == 1);
    CHECK(sink.lastTick.removed);

    sink.clear();
    CHECK(sink.tradeCount == 0);
    CHECK(sink.trades.empty());
}

TEST(event_sink_polymorphic_via_base_pointer) {
    CountingEventSink concrete;
    IEventSink* base = &concrete;
    Trade t;
    t.tradeId = 99;
    base->onTrade(t);
    CHECK(concrete.tradeCount == 1);
    CHECK(concrete.lastTrade.tradeId == 99);
}
