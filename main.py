from ingest.trade_stream import Tradestream
from ingest.orderbook_stream import Orderbookstream
import asyncio
import signal
import sys

async def main():
    ob = Orderbookstream(symbol="BTC")
    ts = Tradestream(symbol="BTC", quote_engine=ob.quote_engine)
    
    def signal_handler(sig, frame):
        print("\n" + "="*80)
        print("PERFORMANCE SUMMARY")
        print("="*80)
        summary = ob.quote_engine.get_session_performance_summary()
        for key, value in summary.items():
            print(f"{key.replace('_', ' ').title()}: {value}")
        print("="*80)
        print("Realistic Python HFT Simulation Benchmarks:")
        print("Excellent: Sharpe ≥ 0.8, Win Rate ≥ 52%, Max DD ≤ 3%, O:T ≤ 15")
        print("Good: Sharpe ≥ 0.5, Win Rate ≥ 50%, Max DD ≤ 5%, O:T ≤ 25")
        print("Acceptable: Sharpe ≥ 0.2, Win Rate ≥ 48%, Max DD ≤ 8%")
        print("="*80)
        sys.exit(0)
    
    signal.signal(signal.SIGINT, signal_handler)

    await asyncio.gather(
        ob.stream_data(),
        ts.load_data()
    )

if __name__ == "__main__":
    asyncio.run(main())