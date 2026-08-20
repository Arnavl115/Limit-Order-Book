"""
Phase 5B/5C — MM config (Python constants, no yaml dep).
All prices in integer ticks, qty in lots.
"""

# Quoting
HALF_SPREAD = 2          # ticks each side from mid
ORDER_QTY = 5            # lots per quote
INVENTORY_SKEW = 0.5     # ticks per inventory unit (shift both quotes down when long)
MIN_SPREAD = 1           # minimum distance between bid and ask

# Risk
MAX_INVENTORY = 50       # absolute position limit
MAX_ORDER_SIZE = 10      # per-order qty cap
MAX_ORDERS_PER_SEC = 20  # throttling (not strict)

# Gateway
GATEWAY_HOST = "127.0.0.1"
GATEWAY_PORT = 9000
REFERENCE_PRICE = 100  # used when book empty (mid fallback)
# For production FastOrderBook domain 1..100000, use 50000; for tests with price 100, 100 is fine.

# Behavior
REFRESH_INTERVAL_MS = 200  # requote interval
HEARTBEAT_TIMEOUT_S = 5.0  # reconnect if no message for this long
DRY_RUN = False
