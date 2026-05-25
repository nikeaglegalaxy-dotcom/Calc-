"""
Скрипт для парсинга топ-500 игроков с сайта https://rivalsmeta.com/leaderboard.

Использует Playwright для обхода защиты от ботов (Cloudflare) и корректной
обработки динамической отрисовки страницы (React/Next.js). Проходит по 5
страницам пагинации (по 100 игроков на каждой), собирает 8 параметров для
каждого игрока, очищает данные и сохраняет результат в CSV-файл.

Установка зависимостей:
    pip install playwright
    playwright install chromium

Запуск:
    python scrape_rivalsmeta.py
"""

import csv
import random
import re
import sys
import time
from typing import Optional

from playwright.sync_api import Page, TimeoutError as PlaywrightTimeoutError, sync_playwright


BASE_URL = "https://rivalsmeta.com/leaderboard"
OUTPUT_CSV = "marvel_rivals_top500.csv"
PAGES_TO_SCRAPE = 5
PLAYERS_PER_PAGE = 100

CSV_HEADERS = [
    "rank",
    "nickname",
    "rank_name",
    "score",
    "games_played",
    "winrate_percent",
    "wins",
    "losses",
]


def clean_int(value: Optional[str]) -> Optional[int]:
    """Убирает запятые/пробелы и нечисловые символы, возвращает int или None."""
    if value is None:
        return None
    digits = re.sub(r"[^\d]", "", value)
    return int(digits) if digits else None


def clean_float(value: Optional[str]) -> Optional[float]:
    """Извлекает число (winrate) из строки вида '63.96%'."""
    if value is None:
        return None
    match = re.search(r"\d+(?:\.\d+)?", value)
    return float(match.group(0)) if match else None


def parse_wl(value: Optional[str]) -> tuple[Optional[int], Optional[int]]:
    """Парсит строку вида '71W / 40L' в (wins, losses)."""
    if value is None:
        return None, None
    wins_match = re.search(r"(\d[\d,]*)\s*W", value, flags=re.IGNORECASE)
    losses_match = re.search(r"(\d[\d,]*)\s*L", value, flags=re.IGNORECASE)
    wins = clean_int(wins_match.group(1)) if wins_match else None
    losses = clean_int(losses_match.group(1)) if losses_match else None
    return wins, losses


def safe_text(row, selector: str) -> Optional[str]:
    """Берёт текст элемента по селектору, возвращает None при отсутствии."""
    try:
        loc = row.locator(selector)
        if loc.count() == 0:
            return None
        text = loc.first.inner_text(timeout=2000)
        return text.strip() if text else None
    except Exception:
        return None


def extract_players_from_page(page: Page) -> list[dict]:
    """Извлекает данные всех игроков с текущей страницы лидерборда."""
    # Ждём появления таблицы. Селекторы подобраны устойчиво к перегенерации
    # CSS-модулей: ищем непосредственно строки, содержащие ссылку на игрока.
    candidate_selectors = [
        "table tbody tr",
        "tr:has(a[href*='/player/'])",
        "[class*='leaderboard'] tr",
        "[class*='table'] [class*='row']:has(a[href*='/player/'])",
    ]

    rows = None
    for sel in candidate_selectors:
        try:
            page.wait_for_selector(sel, timeout=15000)
            loc = page.locator(sel)
            if loc.count() >= 10:
                rows = loc
                break
        except PlaywrightTimeoutError:
            continue

    if rows is None or rows.count() == 0:
        print("  [!] Не удалось найти строки таблицы на странице.", file=sys.stderr)
        return []

    players: list[dict] = []
    total = rows.count()
    for i in range(total):
        row = rows.nth(i)
        try:
            cells_text: list[str] = []
            cells = row.locator("td")
            cell_count = cells.count()
            if cell_count == 0:
                # На случай не-table вёрстки — берём весь текст и разбиваем по строкам.
                whole = row.inner_text(timeout=2000)
                cells_text = [c.strip() for c in whole.split("\n") if c.strip()]
            else:
                for c in range(cell_count):
                    try:
                        cells_text.append(cells.nth(c).inner_text(timeout=2000).strip())
                    except Exception:
                        cells_text.append("")

            joined = " | ".join(cells_text)

            rank_val: Optional[int] = None
            nickname: Optional[str] = None
            rank_name: Optional[str] = None
            score: Optional[int] = None
            games_played: Optional[int] = None
            winrate: Optional[float] = None
            wins: Optional[int] = None
            losses: Optional[int] = None

            # Место — обычно первая ячейка с числом.
            for txt in cells_text:
                if re.fullmatch(r"#?\s*\d+", txt):
                    rank_val = clean_int(txt)
                    break

            # Никнейм — берём из ссылки на профиль игрока.
            try:
                link = row.locator("a[href*='/player/']").first
                if link.count() > 0:
                    nickname = link.inner_text(timeout=2000).strip()
            except Exception:
                nickname = None
            if not nickname and len(cells_text) >= 2:
                nickname = cells_text[1]

            # W/L — ищем по характерному шаблону "NN W / NN L".
            wl_text = next(
                (t for t in cells_text if re.search(r"\d+\s*W\s*/\s*\d+\s*L", t, re.I)),
                None,
            )
            if wl_text:
                wins, losses = parse_wl(wl_text)

            # Winrate — ищем процент.
            wr_text = next((t for t in cells_text if "%" in t), None)
            winrate = clean_float(wr_text) if wr_text else None

            # Очки и сыгранные игры — оба чистые числа (могут быть с запятой).
            numeric_candidates = []
            for t in cells_text:
                if not t:
                    continue
                if re.fullmatch(r"#?\s*\d+", t):
                    continue  # это место в топе
                if "%" in t or "W" in t.upper() or "L" in t.upper():
                    continue
                if re.fullmatch(r"[\d,\.\s]+", t):
                    val = clean_int(t)
                    if val is not None:
                        numeric_candidates.append(val)

            if numeric_candidates:
                score = max(numeric_candidates)
                remaining = [v for v in numeric_candidates if v != score]
                if remaining:
                    games_played = max(remaining)

            # Если games_played не нашёлся как отдельное число, попробуем wins+losses.
            if games_played is None and wins is not None and losses is not None:
                games_played = wins + losses

            # Ранг — строка вида "One Above All", "Eternity", "Grandmaster" и т. п.,
            # не содержит цифр и знаков процента.
            for t in cells_text:
                if not t or t == nickname:
                    continue
                if "%" in t or "/" in t:
                    continue
                if re.search(r"\d", t):
                    continue
                if len(t) > 40:
                    continue
                rank_name = t
                break

            players.append(
                {
                    "rank": rank_val,
                    "nickname": nickname,
                    "rank_name": rank_name,
                    "score": score,
                    "games_played": games_played,
                    "winrate_percent": winrate,
                    "wins": wins,
                    "losses": losses,
                }
            )
        except Exception as exc:
            print(f"  [!] Ошибка при разборе строки {i}: {exc}", file=sys.stderr)
            players.append(
                {h: None for h in CSV_HEADERS}
            )

    return players


def scrape_all() -> list[dict]:
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
            user_agent=(
                "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
            ),
            viewport={"width": 1440, "height": 900},
            locale="en-US",
        )
        page = context.new_page()

        for page_num in range(1, PAGES_TO_SCRAPE + 1):
            url = BASE_URL if page_num == 1 else f"{BASE_URL}?page={page_num}"
            print(f"[+] Загружаю страницу {page_num}: {url}")
            try:
                page.goto(url, wait_until="domcontentloaded", timeout=60000)
                # Дополнительно ждём, пока таблица отрисуется JS-ом.
                try:
                    page.wait_for_load_state("networkidle", timeout=20000)
                except PlaywrightTimeoutError:
                    pass
            except PlaywrightTimeoutError:
                print(f"  [!] Таймаут при загрузке страницы {page_num}", file=sys.stderr)
                continue

            players = extract_players_from_page(page)
            print(f"  [=] Найдено игроков: {len(players)}")
            all_players.extend(players)

            if page_num < PAGES_TO_SCRAPE:
                delay = random.uniform(2.0, 5.0)
                print(f"  [~] Пауза {delay:.1f}s перед следующей страницей")
                time.sleep(delay)

        browser.close()

    return all_players


def save_csv(rows: list[dict], path: str) -> None:
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_HEADERS)
        writer.writeheader()
        for row in rows:
            writer.writerow({h: ("" if row.get(h) is None else row.get(h)) for h in CSV_HEADERS})


def main() -> int:
    players = scrape_all()
    print(f"[+] Всего собрано записей: {len(players)}")
    save_csv(players, OUTPUT_CSV)
    print(f"[+] Сохранено в {OUTPUT_CSV}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
