#ifndef __GENERATOR_H__
#define __GENERATOR_H__

#include "recompiler/context.h"
#include "operations.h"

namespace N64Recomp {
    struct InstructionContext {
        int rd;
        int rs;
        int rt;
        int sa;

        int fd;
        int fs;
        int ft;

        int cop1_cs;

        uint16_t imm16;

        bool reloc_tag_as_reference;
        RelocType reloc_type;
        uint32_t reloc_section_index;
        uint32_t reloc_target_section_offset;
    };

    class Generator {
    public:
        virtual void process_binary_op(const BinaryOp& op, const InstructionContext& ctx) const = 0;
        virtual void process_unary_op(const UnaryOp& op, const InstructionContext& ctx) const = 0;
        virtual void process_store_op(const StoreOp& op, const InstructionContext& ctx) const = 0;
        virtual void emit_function_start(const std::string& function_name, size_t func_index) const = 0;
        virtual void emit_function_end() const = 0;
        virtual void emit_function_call_lookup(uint32_t addr) const = 0;
        virtual void emit_function_call_by_register(int reg) const = 0;
        // target_section_offset can each be deduced from symbol_index if the full context is available,
        // but for live recompilation the reference symbol list is unavailable so it's still provided.
        virtual void emit_function_call_reference_symbol(const Context& context, uint16_t section_index, size_t symbol_index, uint32_t target_section_offset) const = 0;
        virtual void emit_function_call(const Context& context, size_t function_index) const = 0;
        virtual void emit_named_function_call(const std::string& function_name) const = 0;
        virtual void emit_goto(const std::string& target) const = 0;
        virtual void emit_label(const std::string& label_name) const = 0;
        virtual void emit_jtbl_addend_declaration(const JumpTable& jtbl, int reg) const = 0;
        virtual void emit_branch_condition(const ConditionalBranchOp& op, const InstructionContext& ctx) const = 0;
        virtual void emit_branch_close() const = 0;
        virtual void emit_switch(const Context& recompiler_context, const JumpTable& jtbl, int reg) const = 0;
        virtual void emit_case(int case_index, const std::string& target_label) const = 0;
        virtual void emit_switch_error(uint32_t instr_vram, uint32_t jtbl_vram) const = 0;
        virtual void emit_switch_close() const = 0;
        virtual void emit_return(const Context& context, size_t func_index) const = 0;
        virtual void emit_check_fr(int fpr) const = 0;
        virtual void emit_check_nan(int fpr, bool is_double) const = 0;
        virtual void emit_cop0_status_read(int reg) const = 0;
        virtual void emit_cop0_status_write(int reg) const = 0;
        virtual void emit_cop1_cs_read(int reg) const = 0;
        virtual void emit_cop1_cs_write(int reg) const = 0;
        virtual void emit_muldiv(InstrId instr_id, int reg1, int reg2) const = 0;
        virtual void emit_syscall(uint32_t instr_vram) const = 0;
        virtual void emit_do_break(uint32_t instr_vram) const = 0;
        virtual void emit_pause_self() const = 0;
        virtual void emit_trigger_event(uint32_t event_index) const = 0;
        virtual void emit_comment(const std::string& comment) const = 0;

        // com.recomp.ps1 (Context::ps1). A generator without PS1 support returns false.
        // A GTE command (cop2 with bit 25 set): the whole instruction word.
        virtual bool emit_ps1_gte_command(uint32_t raw) const { (void)raw; return false; }
        // mfc2 / cfc2 (to_gpr) or mtc2 / ctc2: a GTE data (control = false) or control register.
        virtual bool emit_ps1_cop2_move(bool to_gpr, bool control, int gpr, int cop2_reg) const { (void)to_gpr; (void)control; (void)gpr; (void)cop2_reg; return false; }
        // lwc2 / swc2: a GTE data register from / to memory at base + imm.
        virtual bool emit_ps1_cop2_memory(bool store, int cop2_reg, int base, int16_t imm) const { (void)store; (void)cop2_reg; (void)base; (void)imm; return false; }
        // mfc0 / mtc0 of any cop0 register, and rfe.
        virtual bool emit_ps1_cop0_move(bool to_gpr, int gpr, int cop0_reg) const { (void)to_gpr; (void)gpr; (void)cop0_reg; return false; }
        virtual bool emit_ps1_rfe() const { return false; }
        // div / divu with the R3000's results for a zero divisor (never a host exception).
        virtual bool emit_ps1_div(bool is_signed, int reg1, int reg2) const { (void)is_signed; (void)reg1; (void)reg2; return false; }
        // com.recomp.ps1: a [[patches.hook]] (its text goes into C output as is). False: print the text.
        virtual bool emit_text_hook(const Context& context, size_t func_index, const std::string& text) const { (void)context; (void)func_index; (void)text; return false; }
    };

    class CGenerator final : Generator {
    public:
        using Generator::emit_text_hook; // com.recomp.ps1: hooks stay text in C output
        CGenerator(std::ostream& output_file) : output_file(output_file) {};
        void process_binary_op(const BinaryOp& op, const InstructionContext& ctx) const final;
        void process_unary_op(const UnaryOp& op, const InstructionContext& ctx) const final;
        void process_store_op(const StoreOp& op, const InstructionContext& ctx) const final;
        void emit_function_start(const std::string& function_name, size_t func_index) const final;
        void emit_function_end() const final;
        void emit_function_call_lookup(uint32_t addr) const final;
        void emit_function_call_by_register(int reg) const final;
        void emit_function_call_reference_symbol(const Context& context, uint16_t section_index, size_t symbol_index, uint32_t target_section_offset) const final;
        void emit_function_call(const Context& context, size_t function_index) const final;
        void emit_named_function_call(const std::string& function_name) const final;
        void emit_goto(const std::string& target) const final;
        void emit_label(const std::string& label_name) const final;
        void emit_jtbl_addend_declaration(const JumpTable& jtbl, int reg) const final;
        void emit_branch_condition(const ConditionalBranchOp& op, const InstructionContext& ctx) const final;
        void emit_branch_close() const final;
        void emit_switch(const Context& recompiler_context, const JumpTable& jtbl, int reg) const final;
        void emit_case(int case_index, const std::string& target_label) const final;
        void emit_switch_error(uint32_t instr_vram, uint32_t jtbl_vram) const final;
        void emit_switch_close() const final;
        void emit_return(const Context& context, size_t func_index) const final;
        void emit_check_fr(int fpr) const final;
        void emit_check_nan(int fpr, bool is_double) const final;
        void emit_cop0_status_read(int reg) const final;
        void emit_cop0_status_write(int reg) const final;
        void emit_cop1_cs_read(int reg) const final;
        void emit_cop1_cs_write(int reg) const final;
        void emit_muldiv(InstrId instr_id, int reg1, int reg2) const final;
        void emit_syscall(uint32_t instr_vram) const final;
        void emit_do_break(uint32_t instr_vram) const final;
        void emit_pause_self() const final;
        void emit_trigger_event(uint32_t event_index) const final;
        void emit_comment(const std::string& comment) const final;
        bool emit_ps1_gte_command(uint32_t raw) const final;
        bool emit_ps1_cop2_move(bool to_gpr, bool control, int gpr, int cop2_reg) const final;
        bool emit_ps1_cop2_memory(bool store, int cop2_reg, int base, int16_t imm) const final;
        bool emit_ps1_cop0_move(bool to_gpr, int gpr, int cop0_reg) const final;
        bool emit_ps1_rfe() const final;
        bool emit_ps1_div(bool is_signed, int reg1, int reg2) const final;
    private:
        void get_operand_string(Operand operand, UnaryOpType operation, const InstructionContext& context, std::string& operand_string) const;
        void get_binary_expr_string(BinaryOpType type, const BinaryOperands& operands, const InstructionContext& ctx, const std::string& output, std::string& expr_string) const;
        void get_notation(BinaryOpType op_type, std::string& func_string, std::string& infix_string) const;
        std::ostream& output_file;
    };
}

#endif
