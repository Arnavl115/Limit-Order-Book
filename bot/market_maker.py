#!/usr/bin/env python3
"""
Phase 5B/5C — Market-maker bot.
- Symmetric quotes around mid with inventory skew.
- Tracks own resting orders, inventory, book snapshot.
- Uses order.replace for requotes (same id, new price).
- Safety: max inventory, max order size, kill-switch, heartbeat watchdog, dry-run.
- Stdlib only.
"""

import argparse
import json
import logging
import time
import random
from collections import defaultdict

try:
    from .mm_client import MMClient
    from . import config as cfg
    from . import strategy as strat
    config = cfg
    strategy = strat
except ImportError:
    from mm_client import MMClient
    import config
    import strategy

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("mm")

class MarketMaker:
    def __init__(self, host=None, port=None, dry_run=None):
        self.host = host or config.GATEWAY_HOST
        self.port = port or config.GATEWAY_PORT
        self.dry_run = config.DRY_RUN if dry_run is None else dry_run
        self.client = MMClient(self.host, self.port, timeout=1.0)
        self.inventory = 0  # net position: buys - sells
        self.book_bids = {}  # price -> qty (from snapshot/ticks)
        self.book_asks = {}
        self.best_bid = None
        self.best_ask = None
        self.last_mid = None
        self.own_bids = {}  # id -> price
        self.own_asks = {}
        self.next_id = 10000
        self.bid_id = None
        self.ask_id = None
        self.last_quote_mid = None
        self.kill = False
        self.last_msg_time = time.time()

    def _next_id(self):
        self.next_id += 1
        return self.next_id

    def _update_book_from_snapshot(self, msg):
        self.book_bids.clear()
        self.book_asks.clear()
        for lvl in msg.get("bids", []):
            self.book_bids[lvl["price"]] = lvl["qty"]
        for lvl in msg.get("asks", []):
            self.book_asks[lvl["price"]] = lvl["qty"]
        self.best_bid = msg.get("bestBid")
        self.best_ask = msg.get("bestAsk")
        if self.best_bid is not None and self.best_ask is not None:
            self.last_mid = (self.best_bid + self.best_ask)//2
        elif self.best_bid is not None:
            self.last_mid = self.best_bid
        elif self.best_ask is not None:
            self.last_mid = self.best_ask

    def _update_book_from_tick(self, msg):
        side = msg.get("side")
        price = msg.get("price")
        qty = msg.get("qty", 0)
        removed = msg.get("removed", False)
        if side == "buy":
            if removed or qty == 0:
                self.book_bids.pop(price, None)
            else:
                self.book_bids[price] = qty
            # recompute best
            self.best_bid = max(self.book_bids.keys()) if self.book_bids else None
        else:
            if removed or qty == 0:
                self.book_asks.pop(price, None)
            else:
                self.book_asks[price] = qty
            self.best_ask = min(self.book_asks.keys()) if self.book_asks else None

    def _handle_execution(self, msg):
        # msg is execution.report
        oid = msg.get("orderId")
        status = msg.get("status")
        # track own orders
        if oid == self.bid_id or oid == self.ask_id:
            if status in ("filled", "cancelled", "rejected"):
                # remove from own
                self.own_bids.pop(oid, None)
                self.own_asks.pop(oid, None)
                if oid == self.bid_id:
                    self.bid_id = None
                if oid == self.ask_id:
                    self.ask_id = None
            elif status in ("partially_filled",):
                # still resting but qty reduced — keep
                pass
            elif status == "resting":
                # now resting — ensure in own map (price from report)
                side = msg.get("side")
                price = msg.get("price")
                if side == "buy":
                    self.own_bids[oid] = price
                else:
                    self.own_asks[oid] = price
        # inventory from fills
        # execution.report does not directly give fill qty, but we can infer from trade messages.
        # We also get trade messages separately to update inventory.
        pass

    def _handle_trade(self, msg):
        # trade: {takerId, makerId, side, price, qty}
        # If we are taker or maker, update inventory
        taker = msg.get("takerId")
        maker = msg.get("makerId")
        side = msg.get("side")  # taker side
        qty = msg.get("qty", 0)
        is_own_taker = (taker == self.bid_id or taker == self.ask_id)
        is_own_maker = (maker == self.bid_id or maker == self.ask_id)
        # But our ids are for quotes; taker could be our quote that crossed (when we placed crossing order)
        # Simpler: if trade involves our ids, update inventory by side
        # We need to know which side we were: if our bid was hit, we bought
        # If our ask was hit, we sold
        # Determine via maker/taker id match
        # For now, if our bid_id was maker and trade side is sell (taker sell hits our bid), we bought
        # If our ask was maker and trade side is buy, we sold
        # If we were taker, side indicates our side
        if is_own_maker:
            if maker == self.bid_id:
                # our bid was hit by sell taker -> we bought
                self.inventory += qty
                log.info("bought %s @%s via maker bid %s inv=%s", qty, msg.get("price"), maker, self.inventory)
            elif maker == self.ask_id:
                self.inventory -= qty
                log.info("sold %s @%s via maker ask %s inv=%s", qty, msg.get("price"), maker, self.inventory)
        if is_own_taker:
            # we were taker
            if side == "buy":
                self.inventory += qty
                log.info("bought %s @%s as taker inv=%s", qty, msg.get("price"), self.inventory)
            else:
                self.inventory -= qty
                log.info("sold %s @%s as taker inv=%s", qty, msg.get("price"), self.inventory)

    def _check_safety(self):
        if abs(self.inventory) > config.MAX_INVENTORY:
            log.warning("kill-switch: inventory %s exceeds %s", self.inventory, config.MAX_INVENTORY)
            self.kill = True
            return False
        return True

    def _flatten(self):
        log.warning("flattening: cancelling all and market to 0")
        # cancel own bids/asks
        for oid in list(self.own_bids.keys()) + list(self.own_asks.keys()):
            if self.dry_run:
                log.info("[dry-run] would cancel %s", oid)
            else:
                self.client.send({"type": "order.cancel", "id": oid})
        self.own_bids.clear()
        self.own_asks.clear()
        self.bid_id = None
        self.ask_id = None
        # market order to flatten inventory
        if self.inventory != 0:
            side = "sell" if self.inventory > 0 else "buy"
            qty = abs(self.inventory)
            if self.dry_run:
                log.info("[dry-run] would market %s %s to flatten", side, qty)
            else:
                oid = self._next_id()
                self.client.send({"type": "order.new", "id": oid, "side": side, "price": 0, "qty": qty, "orderType": "market"})
            self.inventory = 0

    def quote(self):
        if self.kill:
            return
        if not self._check_safety():
            self._flatten()
            return
        mid = strategy.compute_mid(self.best_bid, self.best_ask, self.last_mid)
        if mid is None:
            log.debug("no mid, skip quote")
            return
        # avoid requote if mid unchanged and we have quotes
        if self.last_quote_mid is not None and abs(mid - self.last_quote_mid) < 1 and self.bid_id is not None and self.ask_id is not None:
            # still check inventory drift may need update
            pass
        bid, ask, bqty, aqty = strategy.compute_quotes(mid, self.inventory, self.best_bid, self.best_ask, self.own_bids, self.own_asks)
        self.last_quote_mid = mid
        # place or replace bid
        if bid is not None:
            if self.bid_id is None:
                nid = self._next_id()
                self.bid_id = nid
                if self.dry_run:
                    log.info("[dry-run] would bid %s x%s id %s inv %s", bid, bqty, nid, self.inventory)
                    self.own_bids[nid] = bid
                else:
                    if strategy.would_cross_self("buy", bid, self.own_bids, self.own_asks, self.best_bid, self.best_ask):
                        log.info("skip bid %s would self-cross", bid)
                    else:
                        self.client.send({"type": "order.new", "id": nid, "side": "buy", "price": bid, "qty": bqty, "orderType": "limit"})
                        self.own_bids[nid] = bid
                        log.info("bid %s x%s id %s", bid, bqty, nid)
            else:
                # replace if price changed
                cur = self.own_bids.get(self.bid_id)
                if cur != bid:
                    if self.dry_run:
                        log.info("[dry-run] would replace bid %s -> %s", cur, bid)
                        self.own_bids[self.bid_id] = bid
                    else:
                        self.client.send({"type": "order.replace", "id": self.bid_id, "price": bid, "qty": bqty})
                        self.own_bids[self.bid_id] = bid
                        log.info("replace bid %s -> %s", cur, bid)
        # ask
        if ask is not None:
            if self.ask_id is None:
                nid = self._next_id()
                self.ask_id = nid
                if self.dry_run:
                    log.info("[dry-run] would ask %s x%s id %s", ask, aqty, nid)
                    self.own_asks[nid] = ask
                else:
                    if strategy.would_cross_self("sell", ask, self.own_bids, self.own_asks, self.best_bid, self.best_ask):
                        log.info("skip ask %s would self-cross", ask)
                    else:
                        self.client.send({"type": "order.new", "id": nid, "side": "sell", "price": ask, "qty": aqty, "orderType": "limit"})
                        self.own_asks[nid] = ask
                        log.info("ask %s x%s id %s", ask, aqty, nid)
            else:
                cur = self.own_asks.get(self.ask_id)
                if cur != ask:
                    if self.dry_run:
                        log.info("[dry-run] would replace ask %s -> %s", cur, ask)
                        self.own_asks[self.ask_id] = ask
                    else:
                        self.client.send({"type": "order.replace", "id": self.ask_id, "price": ask, "qty": aqty})
                        self.own_asks[self.ask_id] = ask
                        log.info("replace ask %s -> %s", cur, ask)

    def run(self, duration_s=None):
        if not self.client.connect():
            log.error("failed to connect")
            return
        # subscribe snapshot
        self.client.send({"type": "subscribe", "channel": "book"})
        # main loop
        start = time.time()
        last_quote = 0
        while True:
            if duration_s and time.time() - start > duration_s:
                break
            if self.kill:
                log.warning("killed, breaking")
                break
            # heartbeat watchdog
            if time.time() - self.last_msg_time > config.HEARTBEAT_TIMEOUT_S:
                log.warning("heartbeat timeout, reconnecting")
                self.client.close()
                if not self.client.connect():
                    log.error("reconnect failed")
                    break
                self.client.send({"type": "subscribe"})
                self.last_msg_time = time.time()
            # poll messages
            msgs = self.client.recv_all(timeout=0.05)
            got = False
            for m in msgs:
                got = True
                self.last_msg_time = time.time()
                t = m.get("type")
                if t == "marketdata.snapshot":
                    self._update_book_from_snapshot(m)
                elif t == "marketdata.tick":
                    self._update_book_from_tick(m)
                elif t == "trade":
                    self._handle_trade(m)
                elif t == "execution.report":
                    self._handle_execution(m)
                elif t == "error":
                    log.warning("server error: %s", m)
                elif t == "pong":
                    pass
            # periodic quote
            now = time.time()*1000
            if now - last_quote > config.REFRESH_INTERVAL_MS:
                self.quote()
                last_quote = now
            # keepalive ping occasionally
            if not got:
                # small sleep to avoid busy loop
                time.sleep(0.02)

        # cleanup
        if not self.dry_run:
            for oid in list(self.own_bids.keys()) + list(self.own_asks.keys()):
                try:
                    self.client.send({"type": "order.cancel", "id": oid})
                except: pass
        self.client.close()
        log.info("run finished, inventory %s", self.inventory)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=config.GATEWAY_HOST)
    ap.add_argument("--port", type=int, default=config.GATEWAY_PORT)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--duration", type=int, default=30, help="seconds to run (0 = infinite)")
    ap.add_argument("--max-inv", type=int, default=config.MAX_INVENTORY)
    args = ap.parse_args()
    if args.dry_run:
        config.DRY_RUN = True
    config.MAX_INVENTORY = args.max_inv
    mm = MarketMaker(host=args.host, port=args.port, dry_run=args.dry_run)
    dur = None if args.duration==0 else args.duration
    mm.run(duration_s=dur)

if __name__ == "__main__":
    main()
