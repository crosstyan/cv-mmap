import asyncio
import zmq
import zmq.asyncio

async def test_sync_reception():
    addr = "ipc:///tmp/cvmmap_camera_5602"
    
    ctx = zmq.asyncio.Context.instance()
    
    # 1. With CONFLATE and specific topic (This should fail to get messages reliably)
    print("--- Test 1: CONFLATE + Specific Topic ---")
    sock1 = ctx.socket(zmq.SUB)
    sock1.setsockopt(zmq.CONFLATE, 1)
    sock1.subscribe(b"\x7d") # FRAME_TOPIC_MAGIC
    sock1.connect(addr)
    
    # 2. No CONFLATE + specific topic (This should work)
    print("--- Test 2: No CONFLATE + Specific Topic ---")
    sock2 = ctx.socket(zmq.SUB)
    sock2.subscribe(b"\x7d")
    sock2.connect(addr)

    try:
        msg1 = await asyncio.wait_for(sock1.recv(), timeout=2.0)
        print("Sock1 received:", len(msg1))
    except asyncio.TimeoutError:
        print("Sock1 timeout (expected due to conflate+filter bug)")

    try:
        msg2 = await asyncio.wait_for(sock2.recv(), timeout=2.0)
        print("Sock2 received:", len(msg2))
    except asyncio.TimeoutError:
        print("Sock2 timeout")
        
    sock1.close()
    sock2.close()

if __name__ == "__main__":
    asyncio.run(test_sync_reception())
