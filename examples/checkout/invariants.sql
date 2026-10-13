-- every order takes one widget out of stock, so these always add up to 100
SELECT i.stock, COUNT(o.id) AS orders
FROM inventory i LEFT JOIN orders o ON o.item = i.item
WHERE i.item = 'widget'
GROUP BY i.item
HAVING i.stock + COUNT(o.id) != 100;
