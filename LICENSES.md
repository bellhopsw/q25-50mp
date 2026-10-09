# Licences and third-party material

*Draft: the author must confirm these choices. This is not legal advice.*

| Part | Licence | Note |
|---|---|---|
| `tools/`, `src/shim/`, `docs/`, guides | MIT (see `LICENSE`) | change if you prefer another licence |
| Sensor driver changes (`src/kernel/`) | GPL-2.0 | the Linux kernel is GPL-2.0, so changes to a kernel module must be published under it, and the **corresponding source must be available** wherever the built module is distributed (the release patch contains it) |
| `lpunpack.py` (https://github.com/unix3dgforce/lpunpack) | LGPL-3.0 | **not bundled**: users download it themselves |
| Nothing Phone 2a JN1 driver | GPL-2.0 | the 50 MP register table was ported from this driver (`custom1` mode) |
| mainline Linux `s5kjn1` driver | GPL-2.0 | used as a reference (cross-check) |
| LineageOS `android_kernel_xelex_mt6789` | GPL-2.0 | base of the kernel module |
| Zinwa firmware (`super.img`, `vendor`, libraries) | proprietary | **not distributed**; only small binary patches are published, and users apply them to their own copy |

Open question for the author: the `.qpatch` files contain changed bytes of Zinwa's vendor libraries. Binary patches are common practice, but consider also documenting the edits as offsets and bytes (see `src/hal/README.md`) and read the firmware's terms.
