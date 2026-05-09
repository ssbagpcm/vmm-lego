# VMM-Lego 🧱

Fork-based VMM proof of concept. Boot one "base kernel" process, clone it with fork() + Linux namespaces for CoW memory isolation — like qcow2 backing files but for processes.

## Build & Run
```bash
gcc -o vmm vmm_base.c -lpthread -Wall -Wextra
./vmm
```

## Commands
- `spawn` — create a new VM clone
- `list` — show running VMs
- `kill <id>` — terminate a VM
- `status <id>` — show VM memory state
- `cowdemo` — demonstrate CoW isolation
