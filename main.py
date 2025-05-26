from ingest.trade_stream import Tradestream
from ingest.orderbook_stream import Orderbookstream
import asyncio

async def main():
    ob = Orderbookstream(symbol="btcusdt")
    ts = Tradestream(symbol="btcusdt", quote_engine=ob.quote_engine)

    await asyncio.gather(
        ob.stream_data(),
        ts.load_data()
    )

if __name__ == "__main__":
    asyncio.run(main())