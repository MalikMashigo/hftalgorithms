# PnL Logging Guide

## File Output

Your bot now creates **pnl_log.txt** which logs all PnL data in real-time.

## View PnL Log

### Real-time monitoring
```bash
tail -f pnl_log.txt
```

### From another terminal while bot is running
```bash
watch -n 1 tail pnl_log.txt
```

### Export to CSV for analysis
```bash
grep "\[PnL\]" pnl_log.txt | grep -v WARNING > pnl_data.csv
```

### Get latest PnL value
```bash
grep "\[PnL\]" pnl_log.txt | tail -1
```

### Find highest PnL achieved
```bash
grep "\[PnL\]" pnl_log.txt | sed 's/.*total=//' | sed 's/ .*//' | sort -n | tail -1
```

### Find lowest PnL achieved
```bash
grep "\[PnL\]" pnl_log.txt | sed 's/.*total=//' | sed 's/ .*//' | sort -n | head -1
```

### Watch for warnings
```bash
grep WARNING pnl_log.txt
```

## What Gets Logged

Each line contains:
```
[PnL] total=XXXXX.XX | positions: sym1=Y sym2=Z sym3=W ...
```

- **total**: Your current mark-to-market PnL in dollars
- **positions**: Net position for each symbol with non-zero position
  - Positive = long
  - Negative = short

## Log Rotation

The file uses **append mode** (`std::ios::app`), so:
- Multiple runs accumulate in the same file
- Each run starts with: `=== PnL Log Started at [timestamp] ===`
- Each run ends with: `=== PnL Log Ended ===`

To start fresh:
```bash
rm pnl_log.txt
```

## Features

✓ Writes to file every 1 second
✓ Flushes immediately (no buffering)
✓ Includes warnings when PnL approaches -5000
✓ Logs both stdout AND file simultaneously
✓ File persists after shutdown
