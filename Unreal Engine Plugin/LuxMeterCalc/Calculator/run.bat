@echo off
REM Launch the LuxMeterCalc Python server on 127.0.0.1:8765.
REM Run "pip install -r requirements.txt" first.
python -m uvicorn server:app --host 127.0.0.1 --port 8765 --reload
