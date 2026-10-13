import os
import sqlite3
import sys

DB = os.path.join(os.path.dirname(__file__), "shop.db")


def init_db(path):
    if os.path.exists(path):
        os.remove(path)
    conn = sqlite3.connect(path)
    conn.executescript("""
        CREATE TABLE inventory (item TEXT PRIMARY KEY, stock INTEGER NOT NULL);
        CREATE TABLE orders (id INTEGER PRIMARY KEY, item TEXT NOT NULL);
        INSERT INTO inventory VALUES ('widget', 100);
    """)
    conn.commit()
    return conn


def place_order(conn, item, crash=False):
    conn.execute("INSERT INTO orders (item) VALUES (?)", (item,))
    conn.commit()

    # bug: the stock update is a separate transaction, so a crash here
    # leaves an order that never took anything out of stock
    if crash:
        os._exit(1)

    conn.execute("UPDATE inventory SET stock = stock - 1 WHERE item = ?", (item,))
    conn.commit()


if __name__ == "__main__":
    crash_on = int(sys.argv[1]) if len(sys.argv) > 1 else None
    conn = init_db(DB)
    for n in range(1, 6):
        place_order(conn, "widget", crash=(n == crash_on))
    print("placed 5 orders")
