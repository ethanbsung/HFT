import os, sys
sys.path.append(os.path.dirname(os.path.dirname(__file__)))
from utils.quote_engine import QuoteEngine


def test_same_price_level_true_less_than_half_tick():
    qe = QuoteEngine()
    assert qe._same_price_level(100.00, 100.004) is True


def test_same_price_level_false_when_over_half_tick():
    qe = QuoteEngine()
    assert qe._same_price_level(100.00, 100.006) is False

