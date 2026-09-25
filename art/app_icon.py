"""Renders the Swapp app icon into assets/icons/.

One monitor whose screen is cut diagonally between the two machines: blue
for the personal one, amber for the work one. Two variants that differ only
in the bezel and stand: dark for light backgrounds, white for dark ones.

Drawn on a 64-unit grid (the geometry of the approved SVG). Every output size
is rendered separately from an 8x supersample rather than downscaled from
256 px, so the 16 px tray icon stays crisp.

    python art/app_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

BLUE = "#1a73e8"
AMBER = "#f29d38"
BEZELS = {"light": "#2b3440", "dark": "#f4f6f9"}

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
PNG_SIZE = 256
SUPERSAMPLE = 8

OUT = Path(__file__).resolve().parent.parent / "assets" / "icons"


def render(size: int, bezel: str) -> Image.Image:
    big = size * SUPERSAMPLE
    s = big / 64

    def pt(x, y):
        return (x * s, y * s)

    image = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle([pt(5, 7), pt(59, 45)], radius=5 * s, fill=bezel)
    draw.polygon([pt(10, 12), pt(38.5, 12), pt(22, 40), pt(10, 40)], fill=BLUE)
    draw.polygon([pt(43, 12), pt(54, 12), pt(54, 40), pt(26.5, 40)], fill=AMBER)
    draw.rectangle([pt(28, 44), pt(36, 52)], fill=bezel)
    draw.rounded_rectangle([pt(19, 51), pt(45, 57)], radius=3 * s, fill=bezel)
    return image.resize((size, size), Image.LANCZOS)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for variant, bezel in BEZELS.items():
        images = [render(size, bezel) for size in ICO_SIZES]
        images[-1].save(
            OUT / f"app-{variant}.ico",
            sizes=[(size, size) for size in ICO_SIZES],
            append_images=images[:-1],
        )
        render(PNG_SIZE, bezel).save(OUT / f"app-{variant}.png")
        print(f"wrote app-{variant}.ico and app-{variant}.png")


if __name__ == "__main__":
    main()
