# hidclass.sys — dead-GUID patch (hide the synthetic twin)

## Why

After the IG↔XI swap the synthetic XInput descriptor lives on the XI_00
interface. hidclass registers that interface under
`GUID_DEVINTERFACE_WINEXINPUT = {6C53D5FD-6480-440F-B618-476750C5E1A6}`
(assigned when device_id contains `WINEXINPUT\` + `&XI_`). Flipping the
last GUID byte to a value no one searches for makes the interface
invisible to XInput, DInput and SDL at once — this is what removes the
double-input twin **without any per-game registry configuration**
(the dinput `Joysticks` disable key became redundant after this patch;
verified by run).

## Recipe

```python
GUID  = bytes.fromhex("fdd5536c80640f44b618476750c5e1a6")  # LE struct order
FAKE  = bytes.fromhex("fdd5536c80640f44b618476750c5e1a7")  # last byte +1

data = open(path, "rb").read()
n = data.count(GUID)
assert n >= 1, f"GUID not found in {path}"
open(path + ".gdbak", "wb").write(data)
open(path, "wb").write(data.replace(GUID, FAKE))
```

## Targets

- `<GE>/files/lib/wine/x86_64-windows/hidclass.sys`
- `<GE>/files/lib/wine/i386-windows/hidclass.sys`

NOTE: patching only the **prefix** copy does NOT work for this driver —
hidclass loads from the dist. (Opposite of winexinput, where the prefix
copy is the one that loads.)

## Side effect (accepted product policy)

XInput is dead for games running on the patched Proton instance. Games
with an SDL fallback (the verified matrix) switch to the real DS4 HID
path and gain PS glyphs. XInput-only games should run on a stock Proton
instance.

Rollback: `mv hidclass.sys.gdbak hidclass.sys` (dist copies).
