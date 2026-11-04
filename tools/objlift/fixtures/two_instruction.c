/* 0x00401000 claimed .text, game/Test/unit.cpp */
void sub_00401000(void) {
    int _fpu_cmp = 0;
    uint32_t _cf = 0;
    int _df = 1;
    uint32_t _flag_k = FK_NONE;
    uint32_t _flag_a = 0, _flag_b = 0;
    uint32_t _itail_tgt = 0;
    RECOMP_REGS_LOCALS
    sfcop_guest_entered(0u); RECOMP_ENTER(0x00401000u);

L_00401000:
    eax = MEM32(0x838010); /* 0x00401000: mov eax, dword ptr [0x838010] */
    RECOMP_FLAGS_OUT(); esp += 4; { RECOMP_REGS_OUT(); return; } /* 0x00401005: ret  */
    RECOMP_REGS_OUT();
}
