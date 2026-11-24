/* AT&T operand grammar. No opcode selection here: address bits, register
 * numbers and sizes become bindings in amd64.dsymspec's encoding programs.
 * `text` points into the parser's temporary, writable line. */
static const DomSymAs_Register *att_gpr(sandstone_asm_t *ctx, const char *text) {
    if (*text != '%') return NULL;
    const DomSymAs_Register *r = domsymas_find_register(ctx->bank, text + 1);
    const DomSymAs_Register *rax = domsymas_find_register(ctx->bank, "rax");
    return r && rax && r->regclass == rax->regclass && r->enc_num < 16 ? r : NULL;
}

static int amd64_att_operand(sandstone_asm_t *ctx, char *text, sand_operand_t *op) {
    op->att = 1;
    op->sib = -1;
    if (*text == '*') { op->indirect = 1; text++; }
    if (*text == '$') {
        op->kind = SAND_OP_IMM;
        return !op->indirect && sand_parse_integer(text+1, &op->ival);
    }
    if (*text == '%') {
        char st[] = "st0";
        const char *name = text + 1;
        if (!strcmp(name, "st")) name = st;
        else if (!strncmp(name, "st(", 3) && strlen(name) == 5 && name[4] == ')' &&
                 name[3] >= '0' && name[3] <= '7') { st[2] = name[3]; name = st; }
        const DomSymAs_Register *r = domsymas_find_register(ctx->bank, name);
        int high = ctx->dsymas_forms && r && r->regclass == 0 && r->width == 8 && r->sub_lo == 8;
        if (!r || r->enc_num >= 16 || (r->sub_lo && !high) ||
            !(att_gpr(ctx,text) || !strncmp(name,"xmm",3) || name == st)) return 0;
        op->kind = SAND_OP_REG; op->width = r->width; op->ival = r->enc_num + (high ? 12 : 0);
        op->regclass = r->regclass;
        op->modrm = 192 + (r->enc_num & 7); op->rex_b = r->enc_num >> 3;
        return 1;
    }
    char *open = strchr(text, '(');
    if (!open) {
        if (op->indirect) return 0;
        char *plt = strchr(text, '@');
        if (plt) {
            if (strcmp(plt, "@PLT")) return 0;
            *plt = 0; op->fixup = 3;
        }
        op->kind = SAND_OP_SYM;
        char *add = strpbrk(text,"+-");
        if (add) {
            if (!sand_parse_integer(add,&op->disp) || op->disp < INT32_MIN || op->disp > INT32_MAX) return 0;
            *add=0;
        }
        if (!sand_is_name(text)) return 0;
        op->sval = sand_strdup(text);
        return op->sval != NULL;
    }
    *open++ = 0;
    char *end = strchr(open, ')');
    if (!end || end[1]) return 0;
    *end = 0;
    char *index = strchr(open, ',');
    int scale = 0, idx = 4, base = -1;
    if (index) {
        *index++ = 0;
        char *s = strchr(index, ',');
        if (s) {
            *s++ = 0;
            int64_t k;
            if (!sand_parse_integer(sand_trim(s), &k) || (k!=1 && k!=2 && k!=4 && k!=8)) return 0;
            scale = k==8 ? 3 : k==4 ? 2 : k==2;
        }
        const DomSymAs_Register *r = att_gpr(ctx,sand_trim(index));
        if (!r || r->width != 64 || r->enc_num == 4) return 0;
        idx = (int)r->enc_num;
        op->rex_x = idx >> 3;
    }
    open = sand_trim(open);
    text = sand_trim(text);
    op->kind = SAND_OP_MEM;
    if (!strcmp(open,"%rip")) {
        if (index) return 0;
        op->modrm = 5; op->disp_size = 4;
        if (!*text || sand_parse_integer(text,&op->disp))
            return op->disp >= INT32_MIN && op->disp <= INT32_MAX;
        char *suffix = strchr(text,'@');
        if (suffix) {
            if (strcmp(suffix,"@GOTPCREL")) return 0;
            *suffix = 0;
        }
        op->fixup = suffix ? 2 : 1;
        char *add = strpbrk(text,"+-");
        if (add) {
            if (!sand_parse_integer(add,&op->disp) || op->disp < INT32_MIN || op->disp > INT32_MAX) return 0;
            *add = 0;
        }
        if (!sand_is_name(text)) return 0;
        op->sval = sand_strdup(text); return op->sval != NULL;
    }
    if (*open) {
        const DomSymAs_Register *r = att_gpr(ctx,open);
        if (!r || r->width != 64) return 0;
        base = (int)r->enc_num;
        op->rex_b = base >> 3;
    } else if (!index) return 0;
    if (*text && (!sand_parse_integer(text,&op->disp) || op->disp < INT32_MIN || op->disp > INT32_MAX)) return 0;
    int mod = 0;
    if (base < 0) op->disp_size = 4;
    else if (op->disp || (base & 7) == 5) {
        mod = op->disp >= -128 && op->disp <= 127 ? 1 : 2;
        op->disp_size = mod == 1 ? 1 : 4;
    }
    int rm = base < 0 ? 5 : base & 7;
    if (index || rm == 4) {
        op->sib = scale * 64 + (idx & 7) * 8 + rm; rm = 4;
    }
    op->modrm = mod * 64 + rm;
    return 1;
}
