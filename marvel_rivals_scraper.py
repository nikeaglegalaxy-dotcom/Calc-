"""
Scraper for the top-500 players on https://rivalsmeta.com/leaderboard.

Uses Playwright (headless Chromium) to bypass Cloudflare and wait for the
React/Next.js leaderboard table to render. Iterates over pages 1..5
(100 players per page = 500 total) and saves a cleaned CSV.

Installation:
    pip install playwright
    playwright install chromium

Run:
    python marvel_rivals_scraper.py
"""

from __future__ import annotations

import csv
import random
import re
import time
from pathlib import Path

from playwright.sync_api import sync_playwright, TimeoutError as PlaywrightTimeoutError


BASE_URL = "https://rivalsmeta.com/leaderboard"
PAGES = range(1, 6)  # 1..5 — 100 players per page
OUTPUT_CSV = Path(__file__).with_name("marvel_rivals_top500.csv")

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36"
)

CSV_HEADERS = [
    "rank",
    "nickname",
    "rank_title",
    "score",
    "games_played",
    "winrate_percent",
    "wins",
    "losses",
]


def clean_number(value: str | None) -> str:
    """Strip commas/spaces and non-digit junk; return digits (and a single dot)."""
    if value is None:
        return ""
    v = value.strip().replace(",", "").replace("\xa0", "").replace(" ", "")
    m = re.search(r"-?\d+(?:\.\d+)?", v)
    return m.group(0) if m else ""


def parse_wl(text: str | None) -> tuple[str, str]:
    """Parse a 'NNW / NNL' (or similar) cell into (wins, losses)."""
    if not text:
        return "", ""
    wins = losses = ""
    m_w = re.search(r"(\d[\d,]*)\s*W", text, flags=re.IGNORECASE)
    m_l = re.search(r"(\d[\d,]*)\s*L", text, flags=re.IGNORECASE)
    if m_w:
        wins = m_w.group(1).replace(",", "")
    if m_l:
        losses = m_l.group(1).replace(",", "")
    # Fallback: two bare numbers separated by '/'
    if not wins and not losses:
        nums = re.findall(r"\d[\d,]*", text)
        if len(nums) >= 2:
            wins = nums[0].replace(",", "")
            losses = nums[1].replace(",", "")
    return wins, losses


def extract_rows(page) -> list[dict]:
    """
    Pull rows out of the leaderboard table.

    Site classes are obfuscated (Next.js + styled-components), so we anchor on
    semantic structure: the leaderboard renders as a <table> with one <tr> per
    player. If the markup shifts, replace the row selector / cell-index
    mapping below.
    """
    # Wait for *some* table row with the expected shape before reading.
    page.wait_for_selector("table tbody tr", timeout=45_000)

    # Pull everything in one evaluate to minimise round-trips.
    raw = page.evaluate(
        """
        () => {
            const rows = Array.from(document.querySelectorAll('table tbody tr'));
            return rows.map(tr => {
                const cells = Array.from(tr.querySelectorAll('td')).map(td => td.innerText.trim());
                // Try to pick out a nickname separately in case the name cell
                // contains rank/avatar text mixed in.
                const nameEl = tr.querySelector('a, [class*="name" i], [class*="player" i]');
                const name = nameEl ? nameEl.innerText.trim() : '';
                return { cells, name };
            });
        }
        """
    )

    players: list[dict] = []
    for entry in raw:
        cells: list[str] = entry.get("cells") or []
        if not cells:
            continue

        # Defensive cell-by-cell extraction. The leaderboard columns are:
        #   0: Rank   1: Player (name + maybe rank icon)   2: Rank title
        #   3: Score  4: Total games  5: Winrate %  6: W / L
        def cell(i: int) -> str | None:
            return cells[i] if i < len(cells) else None

        try:
            rank = clean_number(cell(0))
            nickname = (entry.get("name") or cell(1) or "").splitlines()[0].strip()
            rank_title = (cell(2) or "").strip()
            score = clean_number(cell(3))
            games_played = clean_number(cell(4))
            winrate = clean_number(cell(5))
            wins, losses = parse_wl(cell(6))
        except Exception:
            rank = nickname = rank_title = score = ""
            games_played = winrate = wins = losses = ""

        # Skip header/garbage rows that slipped through.
        if not rank and not nickname:
            continue

        players.append(
            {
                "rank": rank,
                "nickname": nickname,
                "rank_title": rank_title,
                "score": score,
                "games_played": games_played,
                "winrate_percent": winrate,
                "wins": wins,
                "losses": losses,
            }
        )

    return players


def scrape() -> list[dict]:
    all_players: list[dict] = []

    with sync_playwright() as p:
        browser = p.chromium.launch(
            headless=True,
            args=[
                "--disable-blink-features=AutomationControlled",
                "--no-sandbox",
            ],
        )
        context = browser.new_context(
            user_agent=USER_AGENT,
            viewport={"width": 1440, "height": 900},
            locale="en-US",
        )
        # Light stealth: hide the webdriver flag.
        context.add_init_script(
            "Object.defineProperty(navigator, 'webdriver', {get: () => undefined});"
        )

        page = context.new_page()

        for page_num in PAGES:
            url = f"{BASE_URL}?page={page_num}"
            print(f"[+] Fetching {url}")
            try:
                page.goto(url, wait_until="domcontentloaded", timeout=60_000)
                rows = extract_rows(page)
            except PlaywrightTimeoutError:
                print(f"[!] Timeout on page {page_num}; skipping")
                rows = []
            except Exception as exc:
                print(f"[!] Failed on page {page_num}: {exc}")
                rows = []

            print(f"    -> {len(rows)} players")
            all_players.extend(rows)

            if page_num != PAGES.stop - 1:
                delay = random.uniform(2.0, 5.0)
                print(f"    sleep {delay:.1f}s")
                time.sleep(delay)

        browser.close()

    return all_players


def save_csv(players: list[dict], path: Path) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_HEADERS)
        writer.writeheader()
        writer.writerows(players)
    print(f"[✓] Wrote {len(players)} rows to {path}")


def main() -> None:
    players = scrape()
    if not players:
        print("[!] No data collected. Site markup may have changed — see comments in extract_rows().")
    save_csv(players, OUTPUT_CSV)


if __name__ == "__main__":
    main()
