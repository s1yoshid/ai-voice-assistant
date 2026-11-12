import io
import asyncio
import openai
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from pydantic_settings import BaseSettings, SettingsConfigDict

class Settings(BaseSettings):
    llm_base_url: str
    llm_model: str
    model_config = SettingsConfigDict(env_file=".env")

settings = Settings()

app = FastAPI()

client = openai.AsyncOpenAI(base_url=f"{settings.llm_base_url}/v1", api_key="no-key-required")

# Test endpoint to confirm that server can connect to external local LLM hosted on a different laptop
@app.get("/testllm")
async def call_llm():
    # use the newer Responses API from OpenAI instead of the old Chat Completions
    try:
        response = await client.responses.create(
            model= {settings.llm_model},
            input="Hello!"
        )
        print(response.output_text)
        return {
            "data": response.output_text,
        }
    except Exception as e:
        logging.error(f"Error: {str(e)}")
        raise HTTPException(
            detail=str(e),
            status_code=500,
        )

@app.get("/")
def read_root():
    return {"Hello": "World"}

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    
    # Define our target based on: 1024 samples * 2 bytes per sample = 2048 bytes
    TARGET_CHUNK_SIZE = 2048
    
    # An in-memory binary stream to aggregate incoming audio data
    audio_buffer = io.BytesIO()
    
    try:
        while True:
            # Receive data from Atom Echo
            message = await websocket.receive()

            binary_data = None
            text_data = None

            if message.get("type") == "websocket.receive":
                # Extract payload safely
                binary_data = message.get("bytes")
                text_data = message.get("text")

            # Raw binary audio chunks
            if binary_data is not None:
                audio_buffer.write(binary_data)

                await websocket.send_text(
                    f"Processed ({len(binary_data)} bytes of audio data on server)"
                )
            
            # Terminator command handling
            elif text_data is not None:
                # Force a flush of whatever is left in memory
                final_audio_payload = audio_buffer.getvalue()

                await websocket.send_text(
                    f"final_audio_playload length: {len(final_audio_payload)}"
                )

                print(f"Total audio bytes received: {len(final_audio_payload)}")
                print(f"Expected duration: {len(final_audio_payload) / 2 / 16000:.2f} seconds")

                if len(final_audio_payload) > 0:                    
                    # --- INTEGRATE YOUR AI PIPELINE HERE ---
                    for i in range(0, len(final_audio_payload), TARGET_CHUNK_SIZE):
                        chunk = final_audio_payload[i:i + TARGET_CHUNK_SIZE]

                        print(f"Sending back {len(chunk)} bytes of data")
        
                        # Send the raw chunk as a binary WebSocket frame
                        await websocket.send_bytes(chunk)
                        
                        # Idk if we need this
                        await asyncio.sleep(0.072)
                    
                    await websocket.send_text("PLAYBACK_DONE")
                    
                # Clear and reset the state instantly for the next interaction
                audio_buffer = io.BytesIO()

    except WebSocketDisconnect:
        print("Client disconnected cleanly.")
    except Exception as e:
        print(f"Error encountered: {e}")
    finally:
        audio_buffer.close()