#!/usr/bin/env python3
"""Make the screenshots in docs/img/ from the demo server (no hardware needed).

    pip install playwright && python3 -m playwright install chromium
    python3 tools/tester_screenshots.py
"""
import asyncio, pathlib, subprocess, sys, time
from playwright.async_api import async_playwright

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMG = ROOT / "docs" / "img"
DEMO = ROOT / "tools" / "tester_demo.py"

async def shoot(pg, name, w, h, full=True):
    await pg.set_viewport_size({"width": w, "height": h})
    await pg.wait_for_timeout(400)
    if full:
        sh = await pg.evaluate("document.documentElement.scrollHeight")
        await pg.set_viewport_size({"width": w, "height": max(h, sh)})
        await pg.wait_for_timeout(200)
    await pg.screenshot(path=str(IMG / name))
    print("  ", name)

async def run(port, ap):
    async with async_playwright() as p:
        b = await p.chromium.launch()
        # (lang, scheme, full set?) - full English dark set, plus one light and one Polish example
        combos = [("en", "dark", True), ("en", "light", False), ("pl", "dark", False)] if not ap else [("en", "dark", True)]
        for lang, scheme, full in combos:
            ctx = await b.new_context(color_scheme=scheme, device_scale_factor=2)
            pg = await ctx.new_page()
            errs = []; pg.on("pageerror", lambda e: errs.append(str(e)))
            await pg.add_init_script(f"localStorage.setItem('lang','{lang}')")
            base = f"http://localhost:{port}/"
            suf = f"-{lang}-{scheme}"
            if ap:
                await pg.goto(base, wait_until="networkidle"); await pg.wait_for_timeout(1500)
                await shoot(pg, "setup-mode-phone.png", 390, 844)
            else:
                kinds = [(1, "bjt"), (3, "diode"), (4, "mosfet"), (5, "resistors"), (6, "capacitor"), (7, "ic")] if full else [(1, "bjt")]
                for rid, tag in kinds:
                    await pg.goto(base + f"#result-{rid}", wait_until="networkidle"); await pg.wait_for_timeout(600)
                    if rid == 1:
                        await pg.click(".info >> nth=0")
                    await shoot(pg, f"measure-{tag}{suf}.png", 1280, 860)
                if full:
                    await shoot(pg, "measure-phone.png", 390, 844)
                    for tab in ["panel", "settings", "api"]:
                        await pg.goto(base + "#" + tab, wait_until="networkidle"); await pg.wait_for_timeout(1500)
                        if tab == "settings":
                            await pg.click("#scan"); await pg.wait_for_timeout(1200)
                        await shoot(pg, f"{tab}.png", 1280, 860)
            if errs: print("PAGE ERRORS:", errs); sys.exit(1)
            await ctx.close()
        await b.close()

def main():
    IMG.mkdir(parents=True, exist_ok=True)
    for ap, port in [(False, 18431), (True, 18432)]:
        srv = subprocess.Popen([sys.executable, str(DEMO), "--port", str(port)] + (["--ap"] if ap else []))
        time.sleep(1.5)
        try: asyncio.run(run(port, ap))
        finally: srv.terminate()

if __name__ == "__main__":
    main()
