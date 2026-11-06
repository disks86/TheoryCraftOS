#!/usr/bin/env python3
"""Generates src/Hardware/Chipset/Intel/IntelGpioLayouts.inc: the GPIO community layouts of the Intel PCHs and SoCs.

  gen_intel_gpio_layouts.py <linux source tree> > src/Hardware/Chipset/Intel/IntelGpioLayouts.inc

The layouts are not typed in. They are read from the Linux pinctrl drivers (drivers/pinctrl/intel/pinctrl-*.c): for every ACPI ID (and _UID where
one ID covers several controllers) the communities with their memory resource (barno), their pin range, and the pad groups with the GPIO number
(the ACPI GpioInt/GpioIo pin) each group starts at. Lynx Point and Cherry View use a different register layout and are left out; so are the parts
whose layout Linux takes from ACPI _DSD (Lunar Lake, Panther Lake: pinctrl-intel-platform.c).
"""
import os
import re
import sys

SKIP = {'pinctrl-lynxpoint.c', 'pinctrl-cherryview.c', 'pinctrl-intel.c', 'pinctrl-intel-platform.c', 'pinctrl-merrifield.c', 'pinctrl-moorefield.c',
        'pinctrl-baytrail.c', 'pinctrl-tangier.c'}
NOMAP, ZERO, MATCH = -1, -2, 0
BASES = {'INTEL_GPIO_BASE_NOMAP': NOMAP, 'INTEL_GPIO_BASE_ZERO': ZERO, 'INTEL_GPIO_BASE_MATCH': MATCH}

# Which processors each driver's IDs belong to, for the comments and the names in the table.
NAMES = {
    'INT344B': 'Sunrise Point-LP', 'INT3451': 'Sunrise Point-H', 'INT345D': 'Sunrise Point-H (Union Point)', 'INT3450': 'Cannon Point-H',
    'INT34BB': 'Cannon Point-LP', 'INT3455': 'Ice Lake PCH-LP', 'INT34C3': 'Ice Lake PCH-N', 'INT34C5': 'Tiger Lake PCH-LP',
    'INT34C6': 'Tiger Lake PCH-H', 'INTC1055': 'Tiger Lake PCH-LP / Alder Lake PCH-P', 'INTC1056': 'Alder Lake PCH-S', 'INTC1057': 'Alder Lake PCH-N',
    'INTC1085': 'Raptor Lake PCH-S', 'INTC105E': 'Meteor Lake SoC', 'INTC1083': 'Arrow Lake-H SoC', 'INTC1082': 'Meteor Lake SoC-S',
    'INTC1084': 'Meteor Point PCH-S (800 series)', 'INT3536': 'Lewisburg', 'INTC1071': 'Emmitsburg', 'INTC3000': 'Denverton', 'INTC3001': 'Cedar Fork',
    'INT34C8': 'Jasper Lake', 'INTC1020': 'Elkhart Lake', 'INT34C4': 'Lakefield', 'INT3452': 'Apollo Lake', 'INT34D1': 'Broxton',
    'INT3453': 'Gemini Lake',
}


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def split_args(s):
    return [a.strip() for a in s.split(',')]


def value(token, defines):
    token = token.strip()
    if token in BASES:
        return BASES[token]
    if token in defines:
        return value(defines[token], defines)
    return int(token, 0)


def parse(path):
    text = strip_comments(open(path, encoding='utf-8').read())
    defines = dict(re.findall(r'^#define\s+(\w+)\s+(-?\w+)\s*$', text, flags=re.M))

    # COMMUNITY macros: name -> (params, 'gpps' | gpp size)
    macros = {}
    for name, params, body in re.findall(r'^#define\s+(\w+)\(([^)]*)\)\s*\\\s*\n\s*(INTEL_COMMUNITY_\w+\([^)]*\))', text, flags=re.M):
        params = split_args(params)
        kind, args = re.match(r'(INTEL_COMMUNITY_\w+)\((.*)\)', body).groups()
        args = split_args(args)
        macros[name] = (params, kind, args)

    padgroups = {}
    for name, body in re.findall(r'static const struct intel_padgroup (\w+)\[\]\s*=\s*\{(.*?)\};', text, flags=re.S):
        groups = []
        for r, s, e, g in re.findall(r'INTEL_GPP\(\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^)]+)\)', body):
            groups.append((value(s, defines), value(e, defines), value(g, defines)))
        padgroups[name] = groups

    communities = {}
    for name, body in re.findall(r'static const struct intel_community (\w+)\[\]\s*=\s*\{(.*?)\};', text, flags=re.S):
        out = []
        for macro, args in re.findall(r'(\w+)\(([^)]*)\)', body):
            if macro not in macros:
                raise SystemExit(f'{path}: community macro {macro} not understood')
            params, kind, margs = macros[macro]
            bound = dict(zip(params, split_args(args)))
            sub = [bound.get(a, a) for a in margs]
            barno, start, end = (value(x, defines) for x in sub[:3])
            if kind == 'INTEL_COMMUNITY_GPPS':
                groups = []
                for s, e, g in padgroups[sub[3]]:
                    groups.append((s, e, s if g == MATCH else 0 if g == ZERO else g))
            elif kind == 'INTEL_COMMUNITY_SIZE':
                size = value(sub[3], defines)
                groups = [(s, min(s + size - 1, end), s) for s in range(start, end + 1, size)]   # gpio_base = base: numbering is the pin number
            else:
                raise SystemExit(f'{path}: {kind} not understood')
            out.append((barno, start, end, groups))
        communities[name] = out

    socs = {}
    for name, body in re.findall(r'static const struct intel_pinctrl_soc_data (\w+)\s*=\s*\{(.*?)\};', text, flags=re.S):
        uid = re.search(r'\.uid\s*=\s*"(\w+)"', body)
        comm = re.search(r'\.communities\s*=\s*(\w+)', body)
        if comm:
            socs[name] = (uid.group(1) if uid else None, communities[comm.group(1)])

    arrays = {}
    for name, body in re.findall(r'static const struct intel_pinctrl_soc_data \*(\w+)\[\]\s*=\s*\{(.*?)\};', text, flags=re.S):
        arrays[name] = re.findall(r'&(\w+)', body)

    layouts = []
    match = re.search(r'static const struct acpi_device_id \w+\[\]\s*=\s*\{(.*?)\};', text, flags=re.S)
    for hid, target in re.findall(r'\{\s*"(\w+)",\s*\(kernel_ulong_t\)&?(\w+)\s*\}', match.group(1) if match else ''):
        for soc in (arrays[target] if target in arrays else [target]):
            uid, comms = socs[soc]
            layouts.append((hid, uid, soc, comms))
    return layouts


def main():
    root = os.path.join(sys.argv[1], 'drivers/pinctrl/intel')
    layouts = []
    for f in sorted(os.listdir(root)):
        if f.startswith('pinctrl-') and f.endswith('.c') and f not in SKIP:
            layouts += [(f,) + l for l in parse(os.path.join(root, f))]

    print('// Generated by scripts/gen_intel_gpio_layouts.py from the Linux pinctrl drivers (drivers/pinctrl/intel). Do not edit.')
    print('// Groups: first pin, last pin, GPIO number of the first pin (-1: no GPIO numbers, the pins are not GPIOs).')
    print('// Communities: memory resource index in the controller\'s _CRS, first pin, last pin, groups.')
    print()
    printed = set()
    for f, hid, uid, soc, comms in layouts:
        if soc in printed:
            continue
        printed.add(soc)
        for i, (bar, s, e, groups) in enumerate(comms):
            print(f'constexpr IntelGpioGroup k_{soc}_{i}[] = {{' + ', '.join(f'{{{a}, {b}, {g}}}' for a, b, g in groups) + '};')
    print()
    seen = set()
    for f, hid, uid, soc, comms in layouts:
        if soc in seen:
            continue
        seen.add(soc)
        print(f'constexpr IntelGpioCommunityLayout k_{soc}[] = {{' +
              ', '.join(f'{{{bar}, {s}, {e}, k_{soc}_{i}}}' for i, (bar, s, e, _) in enumerate(comms)) + '};')
    print()
    print('constexpr IntelGpioLayout kIntelGpioLayouts[] = {')
    for f, hid, uid, soc, comms in layouts:
        name = NAMES.get(hid, hid)
        uid_text = f'"{uid}"' if uid else 'nullptr'
        print(f'    {{"{hid}", {uid_text}, "{name}", k_{soc}}},   // {f}')
    print('};')


if __name__ == '__main__':
    main()
