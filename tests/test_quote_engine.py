import os, sys
ROOT_DIR = os.path.dirname(os.path.dirname(__file__))
sys.path.append(ROOT_DIR)
sys.path.append(os.path.join(ROOT_DIR, 'utils'))
from utils.quote_engine import QuoteEngine


def test_same_price_level_true_less_than_half_tick():
    qe = QuoteEngine()
    assert qe._same_price_level(100.00, 100.004) is True


def test_same_price_level_false_when_over_half_tick():
    qe = QuoteEngine()
    assert qe._same_price_level(100.00, 100.006) is False


def test_order_rate_limit_blocks_orders():
    from datetime import datetime, timezone

    qe = QuoteEngine()
    # Set a very low order rate limit for testing
    qe.risk_manager.limits.max_orders_per_second = 2
    qe.risk_manager.order_window_seconds = 1

    orderbook = {
        "bids": [(100.0, 1.0)],
        "asks": [(101.0, 1.0)],
        "timestamp": datetime.now(timezone.utc),
    }

    assert qe.place_order("buy", 100.0, 0.01, orderbook) is True
    assert qe.place_order("buy", 100.0, 0.01, orderbook) is True
    # Third attempt in the same window should be blocked by rate limit
    assert qe.place_order("buy", 100.0, 0.01, orderbook) is False

