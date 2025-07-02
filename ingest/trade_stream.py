import pandas as pd
import asyncio
import json
from datetime import datetime, timezone
import os
from coinbase.websocket import WSClient

class Tradestream:
    def __init__(self, symbol: str = "BTC-USD", quote_engine=None, exec_sim=None, api_key: str = None, api_secret: str = None, key_file: str = None):
        self._symbol = symbol
        self.quote_engine = quote_engine
        self.exec_sim = exec_sim
        self.api_key = api_key
        self.api_secret = api_secret
        self.key_file = key_file
        self.trades = []
        self.batch_size = 1000
        self.file_counter = 0
        self.ws_client = None
        self.running = False
        
        # CRITICAL FIX: Track asyncio tasks to prevent memory leaks
        self.background_tasks = set()  # Keep references to running tasks
        
        # Create data directory if it doesn't exist
        os.makedirs("data/trades", exist_ok=True)

    async def load_data(self):
        """Load historical data (placeholder - not implemented for live trading)"""
        # This would load historical trade data if needed
        # For live trading, we'll use real-time WebSocket data
        pass

    async def stream_data(self):
        """Stream real-time trade data from Coinbase Advanced Trade WebSocket with reconnection"""
        
        if not self.key_file and (not self.api_key or not self.api_secret):
            print("❌ ERROR: API credentials required for trade stream")
            return
        
        print(f"🚀 Starting Coinbase trade stream for {self._symbol}")
        
        def on_message(msg):
            try:
                if isinstance(msg, dict):
                    message = msg
                elif isinstance(msg, str):
                    message = json.loads(msg)
                else:
                    message = msg
                
                self._handle_trade_message(message)
                
            except Exception as e:
                print(f"❌ Error processing trade message: {e}")
                print(f"🚨 Raw message: {msg}")
        
        def on_open():
            print(f"✅ Trade WebSocket connection opened for {self._symbol}")
        
        def on_close():
            print(f"❌ Trade WebSocket connection closed for {self._symbol}")
        
        self.running = True
        reconnect_attempts = 0
        max_reconnect_attempts = 10
        
        while self.running and reconnect_attempts < max_reconnect_attempts:
            try:
                print(f"🔄 Trade stream connection attempt {reconnect_attempts + 1}/{max_reconnect_attempts}")
                
                # Create WebSocket client for trade data
                # Only pass non-None parameters to avoid conflicts
                ws_kwargs = {
                    'on_message': on_message,
                    'on_open': on_open,
                    'on_close': on_close,
                    'verbose': False  # Less verbose for trades
                }
                
                if self.key_file:
                    ws_kwargs['key_file'] = self.key_file
                    ws_kwargs['api_key'] = None  # Explicitly set to None to override defaults
                    ws_kwargs['api_secret'] = None
                elif self.api_key and self.api_secret:
                    ws_kwargs['api_key'] = self.api_key
                    ws_kwargs['api_secret'] = self.api_secret
                
                self.ws_client = WSClient(**ws_kwargs)
                
                # Open connection
                self.ws_client.open()
                
                # Subscribe to market_trades channel for actual trade data
                self.ws_client.market_trades(product_ids=[self._symbol])
                print(f"📈 Subscribed to market trades for {self._symbol}")
                
                # Reset reconnect counter on successful connection
                reconnect_attempts = 0
                
                # Keep connection alive
                while self.running:
                    try:
                        self.ws_client.sleep_with_exception_check(sleep=1)
                    except Exception as e:
                        print(f"❌ Trade WebSocket error: {e}")
                        print(f"🔄 Will attempt to reconnect trade stream in 5 seconds...")
                        break  # Break inner loop to trigger reconnection
                        
            except Exception as e:
                print(f"❌ Failed to start trade WebSocket: {e}")
                reconnect_attempts += 1
                if reconnect_attempts < max_reconnect_attempts:
                    wait_time = min(5 * reconnect_attempts, 30)  # Exponential backoff, max 30s
                    print(f"🔄 Trade stream reconnection attempt {reconnect_attempts}/{max_reconnect_attempts} in {wait_time}s...")
                    await asyncio.sleep(wait_time)
                else:
                    print(f"❌ Max trade stream reconnection attempts ({max_reconnect_attempts}) reached. Giving up.")
                    break
            finally:
                if self.ws_client:
                    try:
                        self.ws_client.close()
                    except:
                        pass
                # Wait before reconnect attempt
                if self.running and reconnect_attempts < max_reconnect_attempts:
                    await asyncio.sleep(5)
    
    def _handle_trade_message(self, message):
        """Handle incoming trade messages"""
        channel = message.get('channel', '')
        
        if channel == 'market_trades':
            events = message.get('events', [])
            for event in events:
                if event.get('type') == 'update':
                    trades = event.get('trades', [])
                    for trade in trades:
                        self._process_trade(trade)
    
    def _process_trade(self, trade):
        """Process individual trade and forward to execution simulator"""
        try:
            product_id = trade.get('product_id')
            if product_id != self._symbol:
                return
            
            trade_price = float(trade.get('price', 0))
            trade_size = float(trade.get('size', 0))
            trade_side = trade.get('side', '')  # 'buy' or 'sell' (taker side)
            trade_time = trade.get('time', '')
            
            # Parse timestamp
            if trade_time:
                try:
                    ts = datetime.fromisoformat(trade_time.replace('Z', '+00:00'))
                except:
                    ts = datetime.now(timezone.utc)
            else:
                ts = datetime.now(timezone.utc)
            
            # Store trade data
            self.trades.append({
                'timestamp': ts,
                'price': trade_price,
                'size': trade_size,
                'side': trade_side,
                'product_id': product_id
            })
            
            # Forward to execution simulator for queue modeling
            if self.exec_sim:
                self.exec_sim.on_trade(trade_price, trade_size, trade_side, ts)
            
            
            print(f"📈 TRADE: {trade_side.upper()} {trade_size:.1f} @ {trade_price:.4f} | Total trades: {len(self.trades)}")
            
            # Save batch periodically with proper task management
            if len(self.trades) >= self.batch_size:
                # CRITICAL FIX: Properly track and clean up asyncio tasks
                task = asyncio.create_task(self._save_trades())
                self.background_tasks.add(task)
                # Clean up task when done to prevent memory leak
                task.add_done_callback(self.background_tasks.discard)
                
        except Exception as e:
            print(f"❌ Error processing trade: {e}")
            print(f"🚨 Trade data: {trade}")
    
    async def _save_trades(self):
        """Save trade data to parquet file"""
        try:
            if not self.trades:
                return
                
            df = pd.DataFrame(self.trades)
            filename = f"data/trades/trades_{self._symbol}_{self.file_counter:06d}.parquet"
            df.to_parquet(filename, index=False)
            
            print(f"💾 Saved {len(self.trades)} trades to {filename}")
            
            self.trades = []
            self.file_counter += 1
            
        except Exception as e:
            print(f"❌ Error saving trades: {e}")
    
    def stop(self):
        """Stop the trade stream"""
        self.running = False
        if self.ws_client:
            self.ws_client.close()
            
        # CRITICAL FIX: Clean up any pending background tasks to prevent memory leaks
        for task in self.background_tasks:
            if not task.done():
                task.cancel()
        self.background_tasks.clear()
        print(f"🧹 Cleaned up {len(self.background_tasks)} background tasks")

    async def main(self):
        """Main entry point for trade streaming"""
        await self.stream_data()
