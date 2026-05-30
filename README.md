# LuxMeterCalc

A toolset developed for a Bachelor's thesis that compares the illuminance (lux)
predicted inside **Unreal Engine 5** from IES light profiles against the
illuminance **physically measured** with a Sekonic light meter that is tracked by
an OptiTrack motion-capture system.

The goal is to build a tool that helps with the planning and previsualization of lighting setups for filmsets. 

## How it works

A filmset is scanned upon visting it in preproduction. The model is then imported in Unreal Engine. There you can use the fixtures from the plugin to prelight the scene. By using the virtual lightmeter from the plugin the illuminance can be measured at any location in the scene.

## How the Dataset was created

A real fixture with a known IES profile illuminates a tracked Sekonic light
meter. The same scene is rebuilt 1:1 in Unreal Engine from the tracked poses.
For every meter position the **predicted** lux (computed from the IES candela
distribution) is compared against the value the **real** meter recorded — across
six physical setups that vary lamp count, room reflectance, and diffusion.

## Repository structure

| Folder | Contents |
|--------|----------|
| `Unreal Engine Plugin/LuxMeterCalc/` | UE5 editor plugin: virtual IES light, light-meter and reflector actors, a comparison UI, and a bundled local **Calculator** service (Python / FastAPI) that performs the photometric math. |
| `Hilfstools/` *(helper tools)* | **ImageAnalyzer** — OCRs a light-meter's display from a video into a CSV / SRT timeline (Tesseract). • **solveTrackingData** — Blender add-on that solves OptiTrack (Motive) rigid-body CSV exports. • **coordinate_converter.html** — browser tool for converting coordinates into UE space. |
| `Messdaten/` *(measurement data)* | Raw and processed data for the six setups (*Aufbau 1–6*): Sekonic readings, OptiTrack tracks, solved poses, IES profiles, calibration runs, and the aggregated `FINALDATA.xlsx`. |

## Quick start — the Calculator service

The Unreal plugin talks to a local photometric service:

```bash
cd "Unreal Engine Plugin/LuxMeterCalc/Calculator"
pip install -r requirements.txt
run.bat   # = python -m uvicorn server:app --host 127.0.0.1 --port 8765 --reload
```

In the UE editor, the plugin's **Ping** button hits `GET /health` and **Measure**
hits `POST /measure`. The photometric model, conventions, and API are documented
in [Calculator/README.md](Unreal%20Engine%20Plugin/LuxMeterCalc/Calculator/README.md).

## Helper tools

```bash
# Read a light-meter display from a video -> CSV -> SRT
cd Hilfstools/ImageAnalyzer
pip install -r requirements.txt
python app.py your_video.mp4 --out dataset.csv --allow-decimal
python csv_to_srt.py dataset.csv --show-frame
```

The Blender add-on lives in `Hilfstools/solveTrackingData/` — install
`blender_addon.zip` via *Blender → Preferences → Add-ons → Install*.

## Requirements

- **Unreal Engine 5** (editor) — for the plugin
- **Python 3.10+** — for the Calculator and ImageAnalyzer
- **Tesseract OCR** and **ffmpeg/ffprobe** — for ImageAnalyzer
- **Blender** — for the tracking solver

## License & citation

Developed as part of a Bachelor's thesis. By Onno at HdM Stuttgart in June 2026.
