from ingest.trade_stream import Tradestream
from ingest.orderbook_stream import Orderbookstream
import asyncio

if __name__ == "__main__":
    ts = Orderbookstream(symbol="btcusdt")
    asyncio.run(ts.stream_data())