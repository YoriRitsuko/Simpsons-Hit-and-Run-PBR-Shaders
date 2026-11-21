.intel_syntax noprefix
.text

# Wrap the name assignment call. Its return address remains the launcher's
# material override site, so both mods keep their own entry points.
.globl _MaterialLoadedHook
_MaterialLoadedHook:
    push ecx
    lea eax, [esp + 0x68]
    mov edx, [esp + 8]
    push eax
    push edx
    push edi
    call _RecordMaterial
    add esp, 12
    pop ecx
    jmp [assign_name]

.globl _MaterialDestroyedHook
_MaterialDestroyedHook:
    push esi
    call _ForgetMaterial
    add esp, 4
    mov dword ptr [esi], 0x5f8790
    jmp [destroy_resume]

.section .rdata,"dr"
assign_name:    .long 0x56f490
destroy_resume: .long 0x571963
