"""
Phase 5B — quoting strategy.
- Symmetric two-sided quotes around mid.
- Inventory skew.
- Self-trade prevention.
"""

try:
    from . import config
except ImportError:
    import config

def compute_mid(best_bid, best_ask, last_mid=None):
    if best_bid is not None and best_ask is not None:
        return (best_bid + best_ask) // 2
    if best_bid is not None:
        return best_bid
    if best_ask is not None:
        return best_ask
    if last_mid is not None:
        return last_mid
    # fallback to reference price when book empty (bootstrap)
    try:
        return config.REFERENCE_PRICE
    except:
        return 100

def compute_quotes(mid, inventory, best_bid=None, best_ask=None, own_bids=None, own_asks=None):
    """
    Returns (bid_price, ask_price, bid_qty, ask_qty) or (None,None,None,None) if cannot quote.
    - mid: integer ticks
    - inventory: net position (long +, short -)
    - best_bid/best_ask: current book best (for self-trade check)
    - own_bids/own_asks: dict id->price of our resting orders
    """
    if mid is None:
        return (None, None, None, None)
    half = config.HALF_SPREAD
    skew = -inventory * config.INVENTORY_SKEW
    # skew is ticks to shift (long => negative => lower quotes)
    skew = int(round(skew))
    bid = mid - half + skew
    ask = mid + half + skew
    # enforce min spread
    if ask - bid < config.MIN_SPREAD:
        # widen symmetrically around mid+skew
        mid_skewed = mid + skew
        bid = mid_skewed - config.MIN_SPREAD // 2
        ask = bid + config.MIN_SPREAD
    # clamp to positive
    if bid <= 0: bid = 1
    if ask <= 0: ask = 1
    # self-trade prevention: don't cross own opposite orders or book best
    # If we have own asks, our bid must be < min(own ask price)
    # If we have own bids, our ask must be > max(own bid price)
    if own_bids:
        max_own_bid = max(own_bids.values()) if own_bids else None
        if max_own_bid is not None and ask <= max_own_bid:
            ask = max_own_bid + 1
    if own_asks:
        min_own_ask = min(own_asks.values()) if own_asks else None
        if min_own_ask is not None and bid >= min_own_ask:
            bid = min_own_ask - 1
            if bid <= 0:
                bid = None
    # also avoid crossing book best (would be immediate taker, not quoting)
    # For a quoting MM, we want to rest, not take. So ensure bid < bestAsk and ask > bestBid
    if best_ask is not None and bid is not None and bid >= best_ask:
        bid = best_ask - 1
        if bid <= 0:
            bid = None
    if best_bid is not None and ask is not None and ask <= best_bid:
        ask = best_bid + 1
    # check qty limits
    qty = config.ORDER_QTY
    if qty > config.MAX_ORDER_SIZE:
        qty = config.MAX_ORDER_SIZE
    # if either side became None, we can still quote one side
    return (bid, ask, qty, qty)

def would_cross_self(side, price, own_bids, own_asks, best_bid=None, best_ask=None):
    if side == "buy":
        # buy would cross if price >= bestAsk or >= min own ask
        if best_ask is not None and price >= best_ask:
            return True
        if own_asks and price >= min(own_asks.values()):
            return True
    else:
        if best_bid is not None and price <= best_bid:
            return True
        if own_bids and price <= max(own_bids.values()):
            return True
    return False
