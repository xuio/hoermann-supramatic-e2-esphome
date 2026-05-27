#!/usr/bin/env python3
"""Generate print-scale A4 ArUco marker PDFs for garage-door video calibration."""

from __future__ import annotations

import argparse
import io
from dataclasses import dataclass
from pathlib import Path

from PIL import Image
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas

try:
    import cv2
except ImportError as err:  # pragma: no cover - dependency check for CLI use
    raise SystemExit(
        "OpenCV with aruco support is required. Run:\n"
        "  uv run --with opencv-contrib-python-headless --with reportlab --with pillow "
        "tools/generate_aruco_marker_pdfs.py"
    ) from err


@dataclass(frozen=True)
class MarkerSpec:
    marker_id: int
    role: str
    placement: str


DEFAULT_MARKERS = [
    MarkerSpec(0, "STATIC", "top-left opening reference"),
    MarkerSpec(1, "STATIC", "top-right opening reference"),
    MarkerSpec(2, "STATIC", "bottom-left opening reference"),
    MarkerSpec(3, "STATIC", "bottom-right opening reference"),
    MarkerSpec(10, "MOVING", "bottom edge of door"),
    MarkerSpec(11, "MOVING SPARE", "optional second moving door marker"),
]


def get_aruco_dictionary(name: str):
    if not hasattr(cv2, "aruco"):
        raise SystemExit("Installed OpenCV does not include cv2.aruco. Use opencv-contrib-python-headless.")
    try:
        dictionary_id = getattr(cv2.aruco, name)
    except AttributeError as err:
        raise SystemExit(f"Unknown ArUco dictionary {name}") from err
    return cv2.aruco.getPredefinedDictionary(dictionary_id)


def marker_image(dictionary, marker_id: int, pixels: int) -> Image.Image:
    if hasattr(cv2.aruco, "generateImageMarker"):
        array = cv2.aruco.generateImageMarker(dictionary, marker_id, pixels, borderBits=1)
    else:
        array = cv2.aruco.drawMarker(dictionary, marker_id, pixels, borderBits=1)
    return Image.fromarray(array).convert("1")


def image_reader(image: Image.Image) -> ImageReader:
    buffer = io.BytesIO()
    image.save(buffer, format="PNG")
    buffer.seek(0)
    return ImageReader(buffer)


def draw_scale_bar(page: canvas.Canvas, x: float, y: float) -> None:
    page.setStrokeColor(colors.black)
    page.setLineWidth(1.2)
    page.line(x, y, x + 100 * mm, y)
    page.line(x, y - 3 * mm, x, y + 3 * mm)
    page.line(x + 50 * mm, y - 2 * mm, x + 50 * mm, y + 2 * mm)
    page.line(x + 100 * mm, y - 3 * mm, x + 100 * mm, y + 3 * mm)
    page.setFont("Helvetica", 8)
    page.drawCentredString(x, y - 7 * mm, "0")
    page.drawCentredString(x + 50 * mm, y - 7 * mm, "50 mm")
    page.drawCentredString(x + 100 * mm, y - 7 * mm, "100 mm")


def draw_marker_page(
    page: canvas.Canvas,
    dictionary,
    dictionary_name: str,
    spec: MarkerSpec,
    marker_size_mm: float,
    dpi: int,
) -> None:
    width, height = A4
    marker_points = marker_size_mm * mm
    marker_pixels = round(marker_size_mm / 25.4 * dpi)
    image = marker_image(dictionary, spec.marker_id, marker_pixels)
    x = (width - marker_points) / 2
    y = 104 * mm

    page.setTitle(f"ArUco {dictionary_name} ID {spec.marker_id}")
    page.setFont("Helvetica-Bold", 18)
    page.drawCentredString(width / 2, height - 24 * mm, f"ArUco {dictionary_name} ID {spec.marker_id}")
    page.setFont("Helvetica-Bold", 12)
    page.drawCentredString(width / 2, height - 34 * mm, f"{spec.role}: {spec.placement}")
    page.setFont("Helvetica", 10)
    page.drawCentredString(width / 2, height - 44 * mm, "Print at 100% / Actual size. Disable 'fit to page'.")
    page.drawCentredString(width / 2, height - 51 * mm, f"Black marker square must measure exactly {marker_size_mm:g} mm.")

    page.setStrokeColor(colors.lightgrey)
    page.setLineWidth(0.4)
    quiet = 7 * mm
    page.rect(x - quiet, y - quiet, marker_points + 2 * quiet, marker_points + 2 * quiet)
    page.drawImage(image_reader(image), x, y, width=marker_points, height=marker_points, mask=None)

    page.setFont("Helvetica", 9)
    page.drawCentredString(width / 2, y - 16 * mm, f"Cut outside the light grey guide. Keep white margin around the marker.")
    draw_scale_bar(page, (width - 100 * mm) / 2, 33 * mm)
    page.showPage()


def write_pdf(
    output_path: Path,
    dictionary,
    dictionary_name: str,
    markers: list[MarkerSpec],
    marker_size_mm: float,
    dpi: int,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    pdf = canvas.Canvas(str(output_path), pagesize=A4)
    for spec in markers:
        draw_marker_page(pdf, dictionary, dictionary_name, spec, marker_size_mm, dpi)
    pdf.save()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate A4 ArUco marker PDFs at exact metric size")
    parser.add_argument("--output-dir", default="docs/markers")
    parser.add_argument("--dictionary", default="DICT_4X4_50")
    parser.add_argument("--marker-size-mm", type=float, default=100.0)
    parser.add_argument("--dpi", type=int, default=600)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    dictionary = get_aruco_dictionary(args.dictionary)
    output_dir = Path(args.output_dir)
    size_label = f"{args.marker_size_mm:g}mm"

    static_markers = [spec for spec in DEFAULT_MARKERS if spec.role == "STATIC"]
    moving_markers = [spec for spec in DEFAULT_MARKERS if spec.role != "STATIC"]

    outputs = [
        (output_dir / f"aruco_{args.dictionary.lower()}_{size_label}_static_reference_tags.pdf", static_markers),
        (output_dir / f"aruco_{args.dictionary.lower()}_{size_label}_moving_door_tags.pdf", moving_markers),
        (output_dir / f"aruco_{args.dictionary.lower()}_{size_label}_all_garage_tracking_tags.pdf", DEFAULT_MARKERS),
    ]
    for path, markers in outputs:
        write_pdf(path, dictionary, args.dictionary, markers, args.marker_size_mm, args.dpi)
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
