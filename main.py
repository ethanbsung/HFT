from ingest.trade_stream import Tradestream
from ingest.orderbook_stream import Orderbookstream
from execution_simulator import ExecutionSimulator
import asyncio
import signal
import sys
import os
from dotenv import load_dotenv

async def main():
    load_dotenv()
    
    api_key = os.getenv('COINBASE_API_KEY')
    api_secret = os.getenv('COINBASE_API_SECRET')
    
    # Now using hybrid approach, can use any symbol
    test_symbol = "DEXT-USD"  # Back to your original symbol
    
    sim = ExecutionSimulator()
    print(f"🚀 Starting market making simulation for {test_symbol}...")
    print(f"📊 Initial state - Position: {sim.position:.6f}, Cash: {sim.cash:.2f}")
    
    ob = Orderbookstream(symbol=test_symbol, exec_sim=sim, api_key=api_key, api_secret=api_secret)
    ts = Tradestream(symbol=test_symbol, quote_engine=ob.quote_engine)
    
    def signal_handler(sig, frame):
        # Use the new comprehensive, readable performance report
        ob.quote_engine.print_comprehensive_performance_report()
        
        # Print simulation results
        if hasattr(ob, 'last_orderbook') and ob.last_orderbook:
            bids = ob.last_orderbook.get('bids', [])
            asks = ob.last_orderbook.get('asks', [])
            if bids and asks:
                best_bid = float(bids[0][0])
                best_ask = float(asks[0][0])
                mid = (best_bid + best_ask) / 2
                print(f"\n📊 EXECUTION SIMULATOR RESULTS:")
                print(f"SIM  PnL: {sim.mark_to_market(mid):.2f}, Pos: {sim.position:.6f}, Cash: {sim.cash:.2f}, Fills: {len(sim.fills)}")
        
        print("\n💡 BENCHMARKS & CONTEXT:")
        print("   Excellent Python HFT: Sharpe ≥ 0.8, Win Rate ≥ 52%, Max DD ≤ 3%, O:T ≤ 15")
        print("   Good Python HFT: Sharpe ≥ 0.5, Win Rate ≥ 50%, Max DD ≤ 5%, O:T ≤ 25")
        print("   Acceptable: Sharpe ≥ 0.2, Win Rate ≥ 48%, Max DD ≤ 8%")
        print("   Note: Production C++ systems achieve 10-100x better latencies")
        sys.exit(0)
    
    signal.signal(signal.SIGINT, signal_handler)

    await asyncio.gather(
        ob.stream_data(),
        ts.load_data()
    )

if __name__ == "__main__":
    asyncio.run(main())