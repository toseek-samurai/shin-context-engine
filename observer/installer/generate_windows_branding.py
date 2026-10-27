"""Refresh checked-in MSI artwork from the project logo (optional: Pillow)."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def main():
    installer = Path(__file__).resolve().parent
    assets = installer / 'assets'
    assets.mkdir(exist_ok=True)
    logo = Image.open(installer / '../../docs/images/shin-logo.png').convert('RGBA')
    logo = logo.crop(logo.getbbox())
    ink, cream, muted = '#192a2d', '#f9ecd5', '#9caca9'

    # WiX expects these exact canvas sizes. Keep the right-hand text area white;
    # all wizard text is rendered by Windows, rather than baked into the bitmap.
    dialog = Image.new('RGB', (493, 312), 'white')
    draw = ImageDraw.Draw(dialog)
    draw.rectangle((0, 0, 163, 311), fill=ink)
    draw.line((26, 260, 137, 260), fill='#405452', width=1)
    mark = logo.resize((112, 112), Image.Resampling.LANCZOS)
    dialog.paste(mark, (26, 43), mark)
    # Only the logotype belongs to the artwork. Segoe UI is supplied by Windows.
    font = ImageFont.truetype('segoeui.ttf', 27)
    draw.text((81, 174), 'SHIN', font=font, fill=cream, anchor='mt')
    font = ImageFont.truetype('segoeui.ttf', 10)
    draw.text((81, 212), 'CONTEXT ENGINE', font=font, fill=muted, anchor='mt')
    dialog.save(assets / 'dialog.bmp')

    banner = Image.new('RGB', (493, 58), 'white')
    draw = ImageDraw.Draw(banner)
    draw.rounded_rectangle((439, 8, 480, 49), radius=9, fill=ink)
    mark = logo.resize((32, 32), Image.Resampling.LANCZOS)
    banner.paste(mark, (444, 13), mark)
    banner.save(assets / 'banner.bmp')

    icon = Image.new('RGBA', (256, 256), ink)
    mark = logo.resize((212, 212), Image.Resampling.LANCZOS)
    icon.alpha_composite(mark, (22, 22))
    icon.save(assets / 'shin.ico', sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])


if __name__ == '__main__':
    main()
