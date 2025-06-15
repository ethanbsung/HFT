import os, sys
ROOT_DIR = os.path.dirname(os.path.dirname(__file__))
sys.path.append(ROOT_DIR)
sys.path.append(os.path.join(ROOT_DIR, 'utils'))
from utils.risk_monitor import RiskMonitor


class DummyRiskManager:
    def __init__(self, emergency=False):
        self._emergency = emergency

    def emergency_risk_shutdown(self):
        return self._emergency


class DummyQuoteEngine:
    def __init__(self, emergency=False):
        self.risk_manager = DummyRiskManager(emergency)


def test_risk_grade_no_emergency():
    qe = DummyQuoteEngine(emergency=False)
    monitor = RiskMonitor(qe)
    grade = monitor._calculate_risk_grade({'active_risk_breaches': []}, 0, None)
    assert grade == 'A'


def test_risk_grade_with_emergency():
    qe = DummyQuoteEngine(emergency=True)
    monitor = RiskMonitor(qe)
    grade = monitor._calculate_risk_grade({'active_risk_breaches': []}, 0, None)
    assert grade == 'C'
