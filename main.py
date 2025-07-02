from ingest.trade_stream import Tradestream
from ingest.orderbook_stream import Orderbookstream
from execution_simulator import ExecutionSimulator
import asyncio
import signal
import sys
import os
from dotenv import load_dotenv

async def main():
    # Use JSON key file instead of environment variables for better PEM handling
    # This avoids issues with multiline PEM keys in .env files
    key_file_path = "cdp_api_key.json"
    
    # Fallback to environment variables if JSON file doesn't exist
    if not os.path.exists(key_file_path):
        load_dotenv()
        api_key = os.getenv('COINBASE_API_KEY')
        api_secret = os.getenv('COINBASE_API_SECRET')
        key_file_path = None  # Don't use key file if falling back to env vars
    else:
        api_key = None
        api_secret = None
    
    # Now using hybrid approach, can use any symbol
    test_symbol = "DEXT-USD"  # Back to your original symbol
    
    sim = ExecutionSimulator()
    print(f"🚀 Starting market making simulation for {test_symbol}...")
    print(f"📊 Initial state - Position: {sim.position:.6f}, Cash: {sim.cash:.2f}")
    print(f"🔧 Debug mode: Watch for these key indicators:")
    print(f"   💰 'FILL P&L' = Spread capture and fees tracking")
    print(f"   📊 'EXEC_SIM: Fill calculation' = Order fill logic")
    print(f"   🔄 'SYNC:' = Order state synchronization")
    print(f"   📈 Status line every ~10s = Overall system health")
    print(f"   🎯 Comprehensive report on exit = Final performance\n")
    
    # Pass key file path if available, otherwise use individual credentials
    if key_file_path:
        ob = Orderbookstream(symbol=test_symbol, exec_sim=sim, key_file=key_file_path)
        ts = Tradestream(symbol=test_symbol, quote_engine=ob.quote_engine, exec_sim=sim, key_file=key_file_path)
    else:
        ob = Orderbookstream(symbol=test_symbol, exec_sim=sim, api_key=api_key, api_secret=api_secret)
        ts = Tradestream(symbol=test_symbol, quote_engine=ob.quote_engine, exec_sim=sim, api_key=api_key, api_secret=api_secret)
    
    # CRITICAL FIX: Ensure ExecutionSimulator callback is properly set up
    # This guarantees order state synchronization between QuoteEngine and ExecutionSimulator
    if hasattr(sim, 'quote_engine_callback') and ob.quote_engine:
        sim.quote_engine_callback = ob.quote_engine._handle_execution_event
        print("✅ ExecutionSimulator callback properly initialized for order state sync")
    else:
        print("❌ WARNING: ExecutionSimulator callback failed to initialize - order sync may fail")
    
    def signal_handler(sig, frame):
        # CRITICAL FIX: Proper resource cleanup before shutdown
        print("\n🛑 Shutdown signal received - cleaning up resources...")
        
        # Stop data streams properly
        try:
            ob.stop()
            print("✅ Orderbook stream stopped")
        except Exception as e:
            print(f"❌ Error stopping orderbook stream: {e}")
            
        try:
            ts.stop()
            print("✅ Trade stream stopped")
        except Exception as e:
            print(f"❌ Error stopping trade stream: {e}")
        
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
                print(f"SIM  PnL: ${sim.mark_to_market(mid):.2f}, Pos: {sim.position:.1f}, Cash: ${sim.cash:.2f}, Fills: {len(sim.fills)}")
                
                # Print fee tier information
                fee_info = sim.get_fee_tier_info()
                print(f"📈 TRADING VOLUME & FEES:")
                print(f"   30-day volume: ${fee_info['volume_30d']:,.0f}")
                print(f"   Current maker fee: {fee_info['current_fee_bps']:.0f}bps ({fee_info['current_maker_fee']:.4f})")
                if fee_info['next_tier_volume']:
                    print(f"   Next tier at: ${fee_info['next_tier_volume']:,.0f} ({fee_info['next_tier_fee']*10000:.0f}bps)")
        
        print("\n💡 BENCHMARKS & CONTEXT:")
        print("   Excellent Python HFT: Sharpe ≥ 0.8, Win Rate ≥ 52%, Max DD ≤ 3%, O:T ≤ 15")
        print("   Good Python HFT: Sharpe ≥ 0.5, Win Rate ≥ 50%, Max DD ≤ 5%, O:T ≤ 25")
        print("   Acceptable: Sharpe ≥ 0.2, Win Rate ≥ 48%, Max DD ≤ 8%")
        print("   Note: Production C++ systems achieve 10-100x better latencies")
        sys.exit(0)
    
    signal.signal(signal.SIGINT, signal_handler)

    await asyncio.gather(
        ob.stream_data(),
        ts.stream_data()
    )

if __name__ == "__main__":
    asyncio.run(main())