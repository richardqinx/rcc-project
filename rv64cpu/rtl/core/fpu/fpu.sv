//     Richard CPU
//     Copyright (C) 2026  Richard Qin

// File path: rtl/core/fpu/fpu_unit.sv
// Description: RV64F/RV64D floating-point execution unit.

module fpu_unit
    import rv64_pkg::*;
(
    input  fp_op_t            fp_op,
    input  fp_fmt_t           fp_fmt,
    input  logic [2:0]        rm,
    input  logic [2:0]        funct3,
    input  logic [4:0]        rs2_addr,
    input  logic [XLEN-1:0]   fs1_data,
    input  logic [XLEN-1:0]   fs2_data,
    input  logic [XLEN-1:0]   fs3_data,
    input  logic [XLEN-1:0]   int_rs1_data,

    output logic [XLEN-1:0]   fp_result,
    output logic [XLEN-1:0]   int_result,
    output logic [4:0]        fflags,
    output logic              illegal_instr
);

    localparam logic [31:0] CANONICAL_NAN_S = 32'h7FC0_0000;
    localparam logic [63:0] CANONICAL_NAN_D = 64'h7FF8_0000_0000_0000;

    logic [31:0] s1_raw;
    logic [31:0] s2_raw;
    logic [31:0] s3_raw;
    logic [31:0] s1;
    logic [31:0] s2;
    logic [31:0] s3;
    logic [63:0] d1;
    logic [63:0] d2;
    logic [63:0] d3;

    assign s1_raw = fs1_data[31:0];
    assign s2_raw = fs2_data[31:0];
    assign s3_raw = fs3_data[31:0];
    assign s1 = (fs1_data[63:32] == 32'hFFFF_FFFF) ? s1_raw : CANONICAL_NAN_S;
    assign s2 = (fs2_data[63:32] == 32'hFFFF_FFFF) ? s2_raw : CANONICAL_NAN_S;
    assign s3 = (fs3_data[63:32] == 32'hFFFF_FFFF) ? s3_raw : CANONICAL_NAN_S;
    assign d1 = fs1_data;
    assign d2 = fs2_data;
    assign d3 = fs3_data;

    function automatic logic is_nan_s(input logic [31:0] value);
        return (&value[30:23]) && (|value[22:0]);
    endfunction

    function automatic logic is_snan_s(input logic [31:0] value);
        return is_nan_s(value) && (value[22] == 1'b0);
    endfunction

    function automatic logic is_nan_d(input logic [63:0] value);
        return (&value[62:52]) && (|value[51:0]);
    endfunction

    function automatic logic is_snan_d(input logic [63:0] value);
        return is_nan_d(value) && (value[51] == 1'b0);
    endfunction

    function automatic logic [9:0] class_s(input logic [31:0] value);
        logic sign;
        logic [7:0] exp;
        logic [22:0] frac;
        sign = value[31];
        exp = value[30:23];
        frac = value[22:0];
        class_s = '0;
        if (&exp && (frac != '0)) begin
            class_s[is_snan_s(value) ? 8 : 9] = 1'b1;
        end else if (sign && (&exp) && (frac == '0)) begin
            class_s[0] = 1'b1;
        end else if (sign && (exp != '0)) begin
            class_s[1] = 1'b1;
        end else if (sign && (exp == '0) && (frac != '0)) begin
            class_s[2] = 1'b1;
        end else if (sign && (exp == '0) && (frac == '0)) begin
            class_s[3] = 1'b1;
        end else if (!sign && (exp == '0) && (frac == '0)) begin
            class_s[4] = 1'b1;
        end else if (!sign && (exp == '0) && (frac != '0)) begin
            class_s[5] = 1'b1;
        end else if (!sign && (exp != '0) && !(&exp)) begin
            class_s[6] = 1'b1;
        end else if (!sign && (&exp) && (frac == '0)) begin
            class_s[7] = 1'b1;
        end
    endfunction

    function automatic logic [9:0] class_d(input logic [63:0] value);
        logic sign;
        logic [10:0] exp;
        logic [51:0] frac;
        sign = value[63];
        exp = value[62:52];
        frac = value[51:0];
        class_d = '0;
        if (&exp && (frac != '0)) begin
            class_d[is_snan_d(value) ? 8 : 9] = 1'b1;
        end else if (sign && (&exp) && (frac == '0)) begin
            class_d[0] = 1'b1;
        end else if (sign && (exp != '0)) begin
            class_d[1] = 1'b1;
        end else if (sign && (exp == '0) && (frac != '0)) begin
            class_d[2] = 1'b1;
        end else if (sign && (exp == '0) && (frac == '0)) begin
            class_d[3] = 1'b1;
        end else if (!sign && (exp == '0) && (frac == '0)) begin
            class_d[4] = 1'b1;
        end else if (!sign && (exp == '0) && (frac != '0)) begin
            class_d[5] = 1'b1;
        end else if (!sign && (exp != '0) && !(&exp)) begin
            class_d[6] = 1'b1;
        end else if (!sign && (&exp) && (frac == '0)) begin
            class_d[7] = 1'b1;
        end
    endfunction

    function automatic logic less_s(input logic [31:0] lhs, input logic [31:0] rhs);
        logic lhs_neg;
        logic rhs_neg;
        lhs_neg = lhs[31];
        rhs_neg = rhs[31];
        if ((lhs[30:0] == '0) && (rhs[30:0] == '0)) begin
            return 1'b0;
        end
        if (lhs_neg != rhs_neg) begin
            return lhs_neg;
        end
        return lhs_neg ? (lhs[30:0] > rhs[30:0]) : (lhs[30:0] < rhs[30:0]);
    endfunction

    function automatic logic less_d(input logic [63:0] lhs, input logic [63:0] rhs);
        logic lhs_neg;
        logic rhs_neg;
        lhs_neg = lhs[63];
        rhs_neg = rhs[63];
        if ((lhs[62:0] == '0) && (rhs[62:0] == '0)) begin
            return 1'b0;
        end
        if (lhs_neg != rhs_neg) begin
            return lhs_neg;
        end
        return lhs_neg ? (lhs[62:0] > rhs[62:0]) : (lhs[62:0] < rhs[62:0]);
    endfunction

    function automatic logic equal_s(input logic [31:0] lhs, input logic [31:0] rhs);
        if ((lhs[30:0] == '0) && (rhs[30:0] == '0)) begin
            return 1'b1;
        end
        return lhs == rhs;
    endfunction

    function automatic logic equal_d(input logic [63:0] lhs, input logic [63:0] rhs);
        if ((lhs[62:0] == '0) && (rhs[62:0] == '0)) begin
            return 1'b1;
        end
        return lhs == rhs;
    endfunction

    function automatic logic [2:0] effective_rm(input logic [2:0] rm_value);
        effective_rm = (rm_value <= 3'b100) ? rm_value : 3'b000;
    endfunction

    function automatic logic round_increment(
        input logic [2:0]      round_mode,
        input logic            sign,
        input logic [127:0]    quotient,
        input logic [127:0]    remainder,
        input logic [127:0]    half
    );
        logic greater_than_half;
        logic equal_to_half;

        greater_than_half = remainder > half;
        equal_to_half     = remainder == half;

        unique case (effective_rm(round_mode))
            3'b000: round_increment = greater_than_half || (equal_to_half && quotient[0]); // RNE
            3'b001: round_increment = 1'b0;                                                // RTZ
            3'b010: round_increment = sign && (remainder != '0);                           // RDN
            3'b011: round_increment = !sign && (remainder != '0);                          // RUP
            3'b100: round_increment = greater_than_half || equal_to_half;                  // RMM
            default: round_increment = 1'b0;
        endcase
    endfunction

    function automatic logic [63:0] invalid_integer_result(
        input logic is_signed,
        input logic is_long,
        input logic sign,
        input logic is_nan
    );
        if (is_signed) begin
            if (sign && !is_nan) begin
                invalid_integer_result = is_long ? 64'h8000_0000_0000_0000 : 64'hFFFF_FFFF_8000_0000;
            end else begin
                invalid_integer_result = is_long ? 64'h7FFF_FFFF_FFFF_FFFF : 64'h0000_0000_7FFF_FFFF;
            end
        end else begin
            if (sign && !is_nan) begin
                invalid_integer_result = '0;
            end else begin
                invalid_integer_result = 64'hFFFF_FFFF_FFFF_FFFF;
            end
        end
    endfunction

    function automatic logic [63:0] cvt_s_to_int(
        input logic [31:0] value,
        input logic [1:0]  int_fmt,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic sign;
        logic [7:0] exp;
        logic [22:0] frac;
        logic [23:0] significand;
        logic signed [12:0] unbiased_exp;
        logic is_signed;
        logic is_long;
        logic invalid;
        logic inexact;
        logic [127:0] magnitude;
        logic [127:0] quotient;
        logic [127:0] remainder;
        logic [127:0] divisor;
        logic [127:0] half;
        logic [127:0] rounded_magnitude;
        logic [63:0] positive_limit;
        logic [63:0] negative_limit;
        logic [63:0] unsigned_limit;
        logic [63:0] integer_value;
        int shift;

        sign          = value[31];
        exp           = value[30:23];
        frac          = value[22:0];
        significand   = (exp == '0) ? {1'b0, frac} : {1'b1, frac};
        unbiased_exp  = $signed({5'b0, exp}) - 13'sd127;
        is_signed     = (int_fmt == 2'd0) || (int_fmt == 2'd2);
        is_long       = (int_fmt == 2'd2) || (int_fmt == 2'd3);
        invalid       = 1'b0;
        inexact       = 1'b0;
        magnitude     = '0;
        quotient      = '0;
        remainder     = '0;
        divisor       = '0;
        half          = '0;
        rounded_magnitude = '0;
        positive_limit = is_long ? 64'h7FFF_FFFF_FFFF_FFFF : 64'h0000_0000_7FFF_FFFF;
        negative_limit = is_long ? 64'h8000_0000_0000_0000 : 64'h0000_0000_8000_0000;
        unsigned_limit = is_long ? 64'hFFFF_FFFF_FFFF_FFFF : 64'h0000_0000_FFFF_FFFF;
        integer_value  = '0;
        flags          = '0;

        if (is_nan_s(value) || (&exp)) begin
            invalid = 1'b1;
        end else if (significand == '0) begin
            rounded_magnitude = '0;
        end else begin
            if (unbiased_exp >= 13'sd23) begin
                shift = int'(unbiased_exp) - 23;
                if (shift >= 104) begin
                    invalid = 1'b1;
                end else begin
                    rounded_magnitude = {104'b0, significand} << shift;
                end
            end else begin
                shift = 23 - int'(unbiased_exp);
                if (shift > 24) begin
                    quotient  = '0;
                    remainder = {104'b0, significand};
                    divisor   = '0;
                    half      = '0;
                    inexact   = remainder != '0;
                    unique case (effective_rm(round_mode))
                        3'b010:  rounded_magnitude = (sign && inexact) ? 128'd1 : 128'd0;
                        3'b011:  rounded_magnitude = (!sign && inexact) ? 128'd1 : 128'd0;
                        default: rounded_magnitude = '0;
                    endcase
                end else begin
                    divisor   = 128'd1 << shift;
                    quotient  = {104'b0, significand} >> shift;
                    remainder = {104'b0, significand} & (divisor - 128'd1);
                    half      = divisor >> 1;
                    inexact   = (remainder != '0);
                    rounded_magnitude = quotient +
                                        (round_increment(round_mode, sign, quotient, remainder, half) ? 128'd1 : 128'd0);
                end
            end
        end

        if (!invalid) begin
            if (is_signed) begin
                if ((!sign && (rounded_magnitude > {64'b0, positive_limit})) ||
                    (sign && (rounded_magnitude > {64'b0, negative_limit}))) begin
                    invalid = 1'b1;
                end
            end else begin
                if ((sign && (rounded_magnitude != '0)) ||
                    (rounded_magnitude > {64'b0, unsigned_limit})) begin
                    invalid = 1'b1;
                end
            end
        end

        if (invalid) begin
            flags[4] = 1'b1;
            return invalid_integer_result(is_signed, is_long, sign, is_nan_s(value));
        end

        if (inexact) begin
            flags[0] = 1'b1;
        end

        integer_value = rounded_magnitude[63:0];
        if (sign) begin
            integer_value = (~integer_value) + 64'd1;
        end

        if (!is_long) begin
            return {{32{integer_value[31]}}, integer_value[31:0]};
        end

        return integer_value;
    endfunction

    function automatic logic [63:0] cvt_d_to_int(
        input logic [63:0] value,
        input logic [1:0]  int_fmt,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic sign;
        logic [10:0] exp;
        logic [51:0] frac;
        logic [52:0] significand;
        logic signed [12:0] unbiased_exp;
        logic is_signed;
        logic is_long;
        logic invalid;
        logic inexact;
        logic [127:0] quotient;
        logic [127:0] remainder;
        logic [127:0] divisor;
        logic [127:0] half;
        logic [127:0] rounded_magnitude;
        logic [63:0] positive_limit;
        logic [63:0] negative_limit;
        logic [63:0] unsigned_limit;
        logic [63:0] integer_value;
        int shift;

        sign          = value[63];
        exp           = value[62:52];
        frac          = value[51:0];
        significand   = (exp == '0) ? {1'b0, frac} : {1'b1, frac};
        unbiased_exp  = $signed({2'b0, exp}) - 13'sd1023;
        is_signed     = (int_fmt == 2'd0) || (int_fmt == 2'd2);
        is_long       = (int_fmt == 2'd2) || (int_fmt == 2'd3);
        invalid       = 1'b0;
        inexact       = 1'b0;
        quotient      = '0;
        remainder     = '0;
        divisor       = '0;
        half          = '0;
        rounded_magnitude = '0;
        positive_limit = is_long ? 64'h7FFF_FFFF_FFFF_FFFF : 64'h0000_0000_7FFF_FFFF;
        negative_limit = is_long ? 64'h8000_0000_0000_0000 : 64'h0000_0000_8000_0000;
        unsigned_limit = is_long ? 64'hFFFF_FFFF_FFFF_FFFF : 64'h0000_0000_FFFF_FFFF;
        integer_value  = '0;
        flags          = '0;

        if (is_nan_d(value) || (&exp)) begin
            invalid = 1'b1;
        end else if (significand == '0) begin
            rounded_magnitude = '0;
        end else begin
            if (unbiased_exp >= 13'sd52) begin
                shift = int'(unbiased_exp) - 52;
                if (shift >= 75) begin
                    invalid = 1'b1;
                end else begin
                    rounded_magnitude = {75'b0, significand} << shift;
                end
            end else begin
                shift = 52 - int'(unbiased_exp);
                if (shift > 53) begin
                    quotient  = '0;
                    remainder = {75'b0, significand};
                    inexact   = remainder != '0;
                    unique case (effective_rm(round_mode))
                        3'b010:  rounded_magnitude = (sign && inexact) ? 128'd1 : 128'd0;
                        3'b011:  rounded_magnitude = (!sign && inexact) ? 128'd1 : 128'd0;
                        default: rounded_magnitude = '0;
                    endcase
                end else begin
                    divisor   = 128'd1 << shift;
                    quotient  = {75'b0, significand} >> shift;
                    remainder = {75'b0, significand} & (divisor - 128'd1);
                    half      = divisor >> 1;
                    inexact   = (remainder != '0);
                    rounded_magnitude = quotient +
                                        (round_increment(round_mode, sign, quotient, remainder, half) ? 128'd1 : 128'd0);
                end
            end
        end

        if (!invalid) begin
            if (is_signed) begin
                if ((!sign && (rounded_magnitude > {64'b0, positive_limit})) ||
                    (sign && (rounded_magnitude > {64'b0, negative_limit}))) begin
                    invalid = 1'b1;
                end
            end else begin
                if ((sign && (rounded_magnitude != '0)) ||
                    (rounded_magnitude > {64'b0, unsigned_limit})) begin
                    invalid = 1'b1;
                end
            end
        end

        if (invalid) begin
            flags[4] = 1'b1;
            return invalid_integer_result(is_signed, is_long, sign, is_nan_d(value));
        end

        if (inexact) begin
            flags[0] = 1'b1;
        end

        integer_value = rounded_magnitude[63:0];
        if (sign) begin
            integer_value = (~integer_value) + 64'd1;
        end

        if (!is_long) begin
            return {{32{integer_value[31]}}, integer_value[31:0]};
        end

        return integer_value;
    endfunction

    function automatic int highest_bit_64(input logic [63:0] value);
        highest_bit_64 = -1;
        for (int bit_idx = 0; bit_idx < 64; bit_idx++) begin
            if (value[bit_idx]) begin
                highest_bit_64 = bit_idx;
            end
        end
    endfunction

    function automatic int highest_bit_53(input logic [52:0] value);
        highest_bit_53 = -1;
        for (int bit_idx = 0; bit_idx < 53; bit_idx++) begin
            if (value[bit_idx]) begin
                highest_bit_53 = bit_idx;
            end
        end
    endfunction

    function automatic logic [31:0] overflow_s_result(input logic sign, input logic [2:0] round_mode);
        unique case (effective_rm(round_mode))
            3'b001:  overflow_s_result = {sign, 8'hFE, 23'h7F_FFFF};
            3'b010:  overflow_s_result = sign ? 32'hFF80_0000 : 32'h7F7F_FFFF;
            3'b011:  overflow_s_result = sign ? 32'hFF7F_FFFF : 32'h7F80_0000;
            default: overflow_s_result = {sign, 8'hFF, 23'b0};
        endcase
    endfunction

    function automatic logic [63:0] overflow_d_result(input logic sign, input logic [2:0] round_mode);
        unique case (effective_rm(round_mode))
            3'b001:  overflow_d_result = {sign, 11'h7FE, 52'hF_FFFF_FFFF_FFFF};
            3'b010:  overflow_d_result = sign ? 64'hFFF0_0000_0000_0000 : 64'h7FEF_FFFF_FFFF_FFFF;
            3'b011:  overflow_d_result = sign ? 64'hFFEF_FFFF_FFFF_FFFF : 64'h7FF0_0000_0000_0000;
            default: overflow_d_result = {sign, 11'h7FF, 52'b0};
        endcase
    endfunction

    function automatic logic [27:0] shift_right_jam_28(
        input logic [27:0] value,
        input int          shift
    );
        logic sticky;

        if (shift <= 0) begin
            return value;
        end
        if (shift >= 28) begin
            return (value != '0) ? 28'd1 : 28'd0;
        end

        sticky = 1'b0;
        for (int bit_idx = 0; bit_idx < 28; bit_idx++) begin
            if (bit_idx < shift) begin
                sticky = sticky | value[bit_idx];
            end
        end
        return (value >> shift) | (sticky ? 28'd1 : 28'd0);
    endfunction

    function automatic logic shifted_out_28(
        input logic [27:0] value,
        input int          shift
    );
        logic sticky;

        if (shift <= 0) begin
            return 1'b0;
        end
        if (shift >= 28) begin
            return value != '0;
        end

        sticky = 1'b0;
        for (int bit_idx = 0; bit_idx < 28; bit_idx++) begin
            if (bit_idx < shift) begin
                sticky = sticky | value[bit_idx];
            end
        end
        return sticky;
    endfunction

    function automatic logic [27:0] shift_right_jam_48(
        input logic [47:0] value,
        input int          shift
    );
        logic sticky;
        logic [27:0] shifted;

        if (shift <= 0) begin
            return value[27:0];
        end
        if (shift >= 48) begin
            return (value != '0) ? 28'd1 : 28'd0;
        end

        sticky = 1'b0;
        for (int bit_idx = 0; bit_idx < 48; bit_idx++) begin
            if (bit_idx < shift) begin
                sticky = sticky | value[bit_idx];
            end
        end
        shifted = 28'((value >> shift));
        return shifted | (sticky ? 28'd1 : 28'd0);
    endfunction

    function automatic logic [56:0] shift_right_jam_57(
        input logic [56:0] value,
        input int          shift
    );
        logic sticky;

        if (shift <= 0) begin
            return value;
        end
        if (shift >= 57) begin
            return (value != '0) ? 57'd1 : 57'd0;
        end

        sticky = 1'b0;
        for (int bit_idx = 0; bit_idx < 57; bit_idx++) begin
            if (bit_idx < shift) begin
                sticky = sticky | value[bit_idx];
            end
        end
        return (value >> shift) | (sticky ? 57'd1 : 57'd0);
    endfunction

    function automatic logic [64:0] shift_right_jam_65(
        input logic [64:0] value,
        input int          shift
    );
        logic sticky;

        if (shift <= 0) begin
            return value;
        end
        if (shift >= 65) begin
            return (value != '0) ? 65'd1 : 65'd0;
        end

        sticky = 1'b0;
        for (int bit_idx = 0; bit_idx < 65; bit_idx++) begin
            if (bit_idx < shift) begin
                sticky = sticky | value[bit_idx];
            end
        end
        return (value >> shift) | (sticky ? 65'd1 : 65'd0);
    endfunction

    function automatic logic [127:0] shift_right_jam_128(
        input logic [127:0] value,
        input int           shift
    );
        logic sticky;

        if (shift <= 0) begin
            return value;
        end
        if (shift >= 128) begin
            return (value != '0) ? 128'd1 : 128'd0;
        end

        sticky = 1'b0;
        for (int bit_idx = 0; bit_idx < 128; bit_idx++) begin
            if (bit_idx < shift) begin
                sticky = sticky | value[bit_idx];
            end
        end
        return (value >> shift) | (sticky ? 128'd1 : 128'd0);
    endfunction

    function automatic logic [56:0] shift_right_jam_128_to_57(
        input logic [127:0] value,
        input int           shift
    );
        logic sticky;
        logic [56:0] shifted;

        if (shift <= 0) begin
            return value[56:0];
        end
        if (shift >= 128) begin
            return (value != '0) ? 57'd1 : 57'd0;
        end

        sticky = 1'b0;
        for (int bit_idx = 0; bit_idx < 128; bit_idx++) begin
            if (bit_idx < shift) begin
                sticky = sticky | value[bit_idx];
            end
        end
        shifted = 57'(value >> shift);
        return shifted | (sticky ? 57'd1 : 57'd0);
    endfunction

    function automatic logic [31:0] round_pack_s(
        input logic                   sign,
        input logic signed [10:0]     unbiased_exp,
        input logic [27:0]            significand,
        input logic                   inexact_before_round,
        input logic [2:0]             round_mode,
        output logic [4:0]            flags
    );
        logic signed [10:0] result_unbiased_exp;
        logic [27:0] result_sig;
        logic round_any;
        logic round_up;
        logic [24:0] rounded_sig;
        logic [7:0] result_exp;
        logic [22:0] result_frac;
        logic tiny_before_round;
        int subnormal_shift;

        flags = '0;
        result_unbiased_exp = unbiased_exp;
        result_sig = significand;
        result_sig[0] = result_sig[0] | inexact_before_round;
        tiny_before_round = 1'b0;

        if (result_unbiased_exp < -11'sd126) begin
            tiny_before_round = 1'b1;
            subnormal_shift = -126 - int'(result_unbiased_exp);
            result_sig = shift_right_jam_28(result_sig, subnormal_shift);
            result_unbiased_exp = -11'sd126;
        end

        round_any = |result_sig[2:0];
        unique case (effective_rm(round_mode))
            3'b000:  round_up = result_sig[2] && (result_sig[1] || result_sig[0] || result_sig[3]);
            3'b001:  round_up = 1'b0;
            3'b010:  round_up = sign && round_any;
            3'b011:  round_up = !sign && round_any;
            3'b100:  round_up = result_sig[2];
            default: round_up = 1'b0;
        endcase

        rounded_sig = {1'b0, result_sig[26:3]} + (round_up ? 25'd1 : 25'd0);
        if (round_any) begin
            flags[0] = 1'b1;
        end
        if (tiny_before_round && flags[0]) begin
            flags[1] = 1'b1;
        end

        if (rounded_sig[24]) begin
            rounded_sig = rounded_sig >> 1;
            result_unbiased_exp = result_unbiased_exp + 11'sd1;
        end

        if (result_unbiased_exp > 11'sd127) begin
            flags[2] = 1'b1;
            flags[0] = 1'b1;
            return overflow_s_result(sign, round_mode);
        end

        if ((result_unbiased_exp == -11'sd126) && !rounded_sig[23]) begin
            result_exp = 8'b0;
            result_frac = rounded_sig[22:0];
            if (flags[0]) begin
                flags[1] = 1'b1;
            end
        end else begin
            result_exp = 8'(result_unbiased_exp + 11'sd127);
            result_frac = rounded_sig[22:0];
        end

        return {sign, result_exp, result_frac};
    endfunction

    function automatic logic [63:0] round_pack_d(
        input logic                   sign,
        input logic signed [12:0]     unbiased_exp,
        input logic [56:0]            significand,
        input logic                   inexact_before_round,
        input logic [2:0]             round_mode,
        output logic [4:0]            flags
    );
        logic signed [12:0] result_unbiased_exp;
        logic [56:0] result_sig;
        logic round_any;
        logic round_up;
        logic [53:0] rounded_sig;
        logic [10:0] result_exp;
        logic [51:0] result_frac;
        logic tiny_before_round;
        int subnormal_shift;

        flags = '0;
        result_unbiased_exp = unbiased_exp;
        result_sig = significand;
        result_sig[0] = result_sig[0] | inexact_before_round;
        tiny_before_round = 1'b0;

        if (result_unbiased_exp < -13'sd1022) begin
            tiny_before_round = 1'b1;
            subnormal_shift = -1022 - int'(result_unbiased_exp);
            result_sig = shift_right_jam_57(result_sig, subnormal_shift);
            result_unbiased_exp = -13'sd1022;
        end

        round_any = |result_sig[2:0];
        unique case (effective_rm(round_mode))
            3'b000:  round_up = result_sig[2] && (result_sig[1] || result_sig[0] || result_sig[3]);
            3'b001:  round_up = 1'b0;
            3'b010:  round_up = sign && round_any;
            3'b011:  round_up = !sign && round_any;
            3'b100:  round_up = result_sig[2];
            default: round_up = 1'b0;
        endcase

        rounded_sig = {1'b0, result_sig[55:3]} + (round_up ? 54'd1 : 54'd0);
        if (round_any) begin
            flags[0] = 1'b1;
        end
        if (tiny_before_round && flags[0]) begin
            flags[1] = 1'b1;
        end

        if (rounded_sig[53]) begin
            rounded_sig = rounded_sig >> 1;
            result_unbiased_exp = result_unbiased_exp + 13'sd1;
        end

        if (result_unbiased_exp > 13'sd1023) begin
            flags[2] = 1'b1;
            flags[0] = 1'b1;
            return overflow_d_result(sign, round_mode);
        end

        if ((result_unbiased_exp == -13'sd1022) && !rounded_sig[52]) begin
            result_exp = 11'b0;
            result_frac = rounded_sig[51:0];
            if (flags[0]) begin
                flags[1] = 1'b1;
            end
        end else begin
            result_exp = 11'(result_unbiased_exp + 13'sd1023);
            result_frac = rounded_sig[51:0];
        end

        return {sign, result_exp, result_frac};
    endfunction

    function automatic logic [31:0] fp_addsub_s(
        input logic [31:0] lhs,
        input logic [31:0] rhs,
        input logic        subtract,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic lhs_sign;
        logic rhs_sign;
        logic result_sign;
        logic [7:0] lhs_exp;
        logic [7:0] rhs_exp;
        logic [22:0] lhs_frac;
        logic [22:0] rhs_frac;
        logic lhs_is_zero;
        logic rhs_is_zero;
        logic lhs_is_inf;
        logic rhs_is_inf;
        logic signed [10:0] lhs_unbiased_exp;
        logic signed [10:0] rhs_unbiased_exp;
        logic signed [10:0] result_unbiased_exp;
        logic [27:0] lhs_sig;
        logic [27:0] rhs_sig;
        logic [27:0] result_sig;
        logic [27:0] larger_sig;
        logic [27:0] smaller_sig;
        logic signed [10:0] larger_exp;
        logic signs_match;
        logic alignment_inexact;
        int exp_diff;

        flags = '0;
        lhs_sign = lhs[31];
        rhs_sign = rhs[31] ^ subtract;
        lhs_exp = lhs[30:23];
        rhs_exp = rhs[30:23];
        lhs_frac = lhs[22:0];
        rhs_frac = rhs[22:0];
        lhs_is_zero = (lhs[30:0] == '0);
        rhs_is_zero = (rhs[30:0] == '0);
        lhs_is_inf = (&lhs_exp) && (lhs_frac == '0);
        rhs_is_inf = (&rhs_exp) && (rhs_frac == '0);
        signs_match = lhs_sign == rhs_sign;
        alignment_inexact = 1'b0;

        if (is_nan_s(lhs) || is_nan_s(rhs)) begin
            flags[4] = is_snan_s(lhs) || is_snan_s(rhs);
            return CANONICAL_NAN_S;
        end

        if (lhs_is_inf || rhs_is_inf) begin
            if (lhs_is_inf && rhs_is_inf && !signs_match) begin
                flags[4] = 1'b1;
                return CANONICAL_NAN_S;
            end
            return lhs_is_inf ? {lhs_sign, 8'hFF, 23'b0} : {rhs_sign, 8'hFF, 23'b0};
        end

        if (lhs_is_zero && rhs_is_zero) begin
            result_sign = signs_match ? lhs_sign : (effective_rm(round_mode) == 3'b010);
            return {result_sign, 31'b0};
        end
        if (lhs_is_zero) begin
            return {rhs_sign, rhs[30:0]};
        end
        if (rhs_is_zero) begin
            return lhs;
        end

        lhs_unbiased_exp = (lhs_exp == '0) ? -11'sd126 : ($signed({3'b0, lhs_exp}) - 11'sd127);
        rhs_unbiased_exp = (rhs_exp == '0) ? -11'sd126 : ($signed({3'b0, rhs_exp}) - 11'sd127);
        lhs_sig = {1'b0, (lhs_exp == '0) ? 1'b0 : 1'b1, lhs_frac, 3'b000};
        rhs_sig = {1'b0, (rhs_exp == '0) ? 1'b0 : 1'b1, rhs_frac, 3'b000};

        if ((lhs_unbiased_exp > rhs_unbiased_exp) ||
            ((lhs_unbiased_exp == rhs_unbiased_exp) && (lhs_sig >= rhs_sig))) begin
            exp_diff = int'(lhs_unbiased_exp) - int'(rhs_unbiased_exp);
            larger_sig = lhs_sig;
            alignment_inexact = shifted_out_28(rhs_sig, exp_diff);
            smaller_sig = shift_right_jam_28(rhs_sig, exp_diff);
            larger_exp = lhs_unbiased_exp;
            result_sign = lhs_sign;
        end else begin
            exp_diff = int'(rhs_unbiased_exp) - int'(lhs_unbiased_exp);
            larger_sig = rhs_sig;
            alignment_inexact = shifted_out_28(lhs_sig, exp_diff);
            smaller_sig = shift_right_jam_28(lhs_sig, exp_diff);
            larger_exp = rhs_unbiased_exp;
            result_sign = rhs_sign;
        end

        result_unbiased_exp = larger_exp;
        if (signs_match) begin
            result_sig = larger_sig + smaller_sig;
            if (result_sig[27]) begin
                result_sig = shift_right_jam_28(result_sig, 1);
                result_unbiased_exp = result_unbiased_exp + 11'sd1;
            end
        end else begin
            result_sig = larger_sig - smaller_sig;
            if (result_sig == '0) begin
                result_sign = effective_rm(round_mode) == 3'b010;
                return {result_sign, 31'b0};
            end
            while (!result_sig[26] && (result_unbiased_exp > -11'sd126)) begin
                result_sig = result_sig << 1;
                result_unbiased_exp = result_unbiased_exp - 11'sd1;
            end
        end

        return round_pack_s(result_sign, result_unbiased_exp, result_sig, alignment_inexact, round_mode, flags);
    endfunction

    function automatic logic [63:0] fp_addsub_d(
        input logic [63:0] lhs,
        input logic [63:0] rhs,
        input logic        subtract,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic lhs_sign;
        logic rhs_sign;
        logic result_sign;
        logic [10:0] lhs_exp;
        logic [10:0] rhs_exp;
        logic [51:0] lhs_frac;
        logic [51:0] rhs_frac;
        logic lhs_is_zero;
        logic rhs_is_zero;
        logic lhs_is_inf;
        logic rhs_is_inf;
        logic signed [12:0] lhs_unbiased_exp;
        logic signed [12:0] rhs_unbiased_exp;
        logic signed [12:0] result_unbiased_exp;
        logic [56:0] lhs_sig;
        logic [56:0] rhs_sig;
        logic [56:0] result_sig;
        logic [56:0] larger_sig;
        logic [56:0] smaller_sig;
        logic signed [12:0] larger_exp;
        logic signs_match;
        int exp_diff;

        flags = '0;
        lhs_sign = lhs[63];
        rhs_sign = rhs[63] ^ subtract;
        lhs_exp = lhs[62:52];
        rhs_exp = rhs[62:52];
        lhs_frac = lhs[51:0];
        rhs_frac = rhs[51:0];
        lhs_is_zero = (lhs[62:0] == '0);
        rhs_is_zero = (rhs[62:0] == '0);
        lhs_is_inf = (&lhs_exp) && (lhs_frac == '0);
        rhs_is_inf = (&rhs_exp) && (rhs_frac == '0);
        signs_match = lhs_sign == rhs_sign;

        if (is_nan_d(lhs) || is_nan_d(rhs)) begin
            flags[4] = is_snan_d(lhs) || is_snan_d(rhs);
            return CANONICAL_NAN_D;
        end

        if (lhs_is_inf || rhs_is_inf) begin
            if (lhs_is_inf && rhs_is_inf && !signs_match) begin
                flags[4] = 1'b1;
                return CANONICAL_NAN_D;
            end
            return lhs_is_inf ? {lhs_sign, 11'h7FF, 52'b0} : {rhs_sign, 11'h7FF, 52'b0};
        end

        if (lhs_is_zero && rhs_is_zero) begin
            result_sign = signs_match ? lhs_sign : (effective_rm(round_mode) == 3'b010);
            return {result_sign, 63'b0};
        end
        if (lhs_is_zero) begin
            return {rhs_sign, rhs[62:0]};
        end
        if (rhs_is_zero) begin
            return lhs;
        end

        lhs_unbiased_exp = (lhs_exp == '0) ? -13'sd1022 : ($signed({2'b0, lhs_exp}) - 13'sd1023);
        rhs_unbiased_exp = (rhs_exp == '0) ? -13'sd1022 : ($signed({2'b0, rhs_exp}) - 13'sd1023);
        lhs_sig = {1'b0, (lhs_exp == '0) ? 1'b0 : 1'b1, lhs_frac, 3'b000};
        rhs_sig = {1'b0, (rhs_exp == '0) ? 1'b0 : 1'b1, rhs_frac, 3'b000};

        if ((lhs_unbiased_exp > rhs_unbiased_exp) ||
            ((lhs_unbiased_exp == rhs_unbiased_exp) && (lhs_sig >= rhs_sig))) begin
            exp_diff = int'(lhs_unbiased_exp) - int'(rhs_unbiased_exp);
            larger_sig = lhs_sig;
            smaller_sig = shift_right_jam_57(rhs_sig, exp_diff);
            larger_exp = lhs_unbiased_exp;
            result_sign = lhs_sign;
        end else begin
            exp_diff = int'(rhs_unbiased_exp) - int'(lhs_unbiased_exp);
            larger_sig = rhs_sig;
            smaller_sig = shift_right_jam_57(lhs_sig, exp_diff);
            larger_exp = rhs_unbiased_exp;
            result_sign = rhs_sign;
        end

        result_unbiased_exp = larger_exp;
        if (signs_match) begin
            result_sig = larger_sig + smaller_sig;
            if (result_sig[56]) begin
                result_sig = shift_right_jam_57(result_sig, 1);
                result_unbiased_exp = result_unbiased_exp + 13'sd1;
            end
        end else begin
            result_sig = larger_sig - smaller_sig;
            if (result_sig == '0) begin
                result_sign = effective_rm(round_mode) == 3'b010;
                return {result_sign, 63'b0};
            end
            while (!result_sig[55] && (result_unbiased_exp > -13'sd1022)) begin
                result_sig = result_sig << 1;
                result_unbiased_exp = result_unbiased_exp - 13'sd1;
            end
        end

        return round_pack_d(result_sign, result_unbiased_exp, result_sig, 1'b0, round_mode, flags);
    endfunction

    function automatic logic [63:0] fp_mul_d(
        input logic [63:0] lhs,
        input logic [63:0] rhs,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic result_sign;
        logic [10:0] lhs_exp;
        logic [10:0] rhs_exp;
        logic [51:0] lhs_frac;
        logic [51:0] rhs_frac;
        logic lhs_is_zero;
        logic rhs_is_zero;
        logic lhs_is_inf;
        logic rhs_is_inf;
        logic signed [12:0] lhs_unbiased_exp;
        logic signed [12:0] rhs_unbiased_exp;
        logic signed [12:0] result_unbiased_exp;
        logic [52:0] lhs_sig;
        logic [52:0] rhs_sig;
        logic [105:0] product;
        logic [56:0] result_sig;
        int lhs_msb;
        int rhs_msb;

        flags = '0;
        result_sign = lhs[63] ^ rhs[63];
        lhs_exp = lhs[62:52];
        rhs_exp = rhs[62:52];
        lhs_frac = lhs[51:0];
        rhs_frac = rhs[51:0];
        lhs_is_zero = (lhs[62:0] == '0);
        rhs_is_zero = (rhs[62:0] == '0);
        lhs_is_inf = (&lhs_exp) && (lhs_frac == '0);
        rhs_is_inf = (&rhs_exp) && (rhs_frac == '0);

        if (is_nan_d(lhs) || is_nan_d(rhs)) begin
            flags[4] = is_snan_d(lhs) || is_snan_d(rhs);
            return CANONICAL_NAN_D;
        end

        if ((lhs_is_inf && rhs_is_zero) || (rhs_is_inf && lhs_is_zero)) begin
            flags[4] = 1'b1;
            return CANONICAL_NAN_D;
        end

        if (lhs_is_inf || rhs_is_inf) begin
            return {result_sign, 11'h7FF, 52'b0};
        end

        if (lhs_is_zero || rhs_is_zero) begin
            return {result_sign, 63'b0};
        end

        if (lhs_exp == '0) begin
            lhs_msb = highest_bit_53({1'b0, lhs_frac});
            lhs_unbiased_exp = 13'(lhs_msb - 1074);
            lhs_sig = {1'b0, lhs_frac} << (52 - lhs_msb);
        end else begin
            lhs_unbiased_exp = $signed({2'b0, lhs_exp}) - 13'sd1023;
            lhs_sig = {1'b1, lhs_frac};
        end

        if (rhs_exp == '0) begin
            rhs_msb = highest_bit_53({1'b0, rhs_frac});
            rhs_unbiased_exp = 13'(rhs_msb - 1074);
            rhs_sig = {1'b0, rhs_frac} << (52 - rhs_msb);
        end else begin
            rhs_unbiased_exp = $signed({2'b0, rhs_exp}) - 13'sd1023;
            rhs_sig = {1'b1, rhs_frac};
        end

        product = lhs_sig * rhs_sig;
        if (product[105]) begin
            result_unbiased_exp = lhs_unbiased_exp + rhs_unbiased_exp + 13'sd1;
            result_sig = shift_right_jam_128_to_57({22'b0, product}, 50);
        end else begin
            result_unbiased_exp = lhs_unbiased_exp + rhs_unbiased_exp;
            result_sig = shift_right_jam_128_to_57({22'b0, product}, 49);
        end

        return round_pack_d(result_sign, result_unbiased_exp, result_sig, 1'b0, round_mode, flags);
    endfunction

    function automatic logic [31:0] fp_mul_s(
        input logic [31:0] lhs,
        input logic [31:0] rhs,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic result_sign;
        logic [7:0] lhs_exp;
        logic [7:0] rhs_exp;
        logic [22:0] lhs_frac;
        logic [22:0] rhs_frac;
        logic lhs_is_zero;
        logic rhs_is_zero;
        logic lhs_is_inf;
        logic rhs_is_inf;
        logic signed [10:0] lhs_unbiased_exp;
        logic signed [10:0] rhs_unbiased_exp;
        logic signed [10:0] result_unbiased_exp;
        logic [23:0] lhs_sig;
        logic [23:0] rhs_sig;
        logic [47:0] product;
        logic [27:0] result_sig;
        int lhs_msb;
        int rhs_msb;

        flags = '0;
        result_sign = lhs[31] ^ rhs[31];
        lhs_exp = lhs[30:23];
        rhs_exp = rhs[30:23];
        lhs_frac = lhs[22:0];
        rhs_frac = rhs[22:0];
        lhs_is_zero = (lhs[30:0] == '0);
        rhs_is_zero = (rhs[30:0] == '0);
        lhs_is_inf = (&lhs_exp) && (lhs_frac == '0);
        rhs_is_inf = (&rhs_exp) && (rhs_frac == '0);

        if (is_nan_s(lhs) || is_nan_s(rhs)) begin
            flags[4] = is_snan_s(lhs) || is_snan_s(rhs);
            return CANONICAL_NAN_S;
        end

        if ((lhs_is_inf && rhs_is_zero) || (rhs_is_inf && lhs_is_zero)) begin
            flags[4] = 1'b1;
            return CANONICAL_NAN_S;
        end

        if (lhs_is_inf || rhs_is_inf) begin
            return {result_sign, 8'hFF, 23'b0};
        end

        if (lhs_is_zero || rhs_is_zero) begin
            return {result_sign, 31'b0};
        end

        if (lhs_exp == '0) begin
            lhs_msb = highest_bit_64({41'b0, lhs_frac});
            lhs_unbiased_exp = 11'(lhs_msb - 149);
            lhs_sig = {1'b0, lhs_frac} << (23 - lhs_msb);
        end else begin
            lhs_unbiased_exp = $signed({3'b0, lhs_exp}) - 11'sd127;
            lhs_sig = {1'b1, lhs_frac};
        end

        if (rhs_exp == '0) begin
            rhs_msb = highest_bit_64({41'b0, rhs_frac});
            rhs_unbiased_exp = 11'(rhs_msb - 149);
            rhs_sig = {1'b0, rhs_frac} << (23 - rhs_msb);
        end else begin
            rhs_unbiased_exp = $signed({3'b0, rhs_exp}) - 11'sd127;
            rhs_sig = {1'b1, rhs_frac};
        end

        product = lhs_sig * rhs_sig;
        if (product[47]) begin
            result_unbiased_exp = lhs_unbiased_exp + rhs_unbiased_exp + 11'sd1;
            result_sig = shift_right_jam_48(product, 21);
        end else begin
            result_unbiased_exp = lhs_unbiased_exp + rhs_unbiased_exp;
            result_sig = shift_right_jam_48(product, 20);
        end

        return round_pack_s(result_sign, result_unbiased_exp, result_sig, 1'b0, round_mode, flags);
    endfunction

    function automatic logic [31:0] fp_div_s(
        input logic [31:0] lhs,
        input logic [31:0] rhs,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic result_sign;
        logic [7:0] lhs_exp;
        logic [7:0] rhs_exp;
        logic [22:0] lhs_frac;
        logic [22:0] rhs_frac;
        logic lhs_is_zero;
        logic rhs_is_zero;
        logic lhs_is_inf;
        logic rhs_is_inf;
        logic signed [10:0] lhs_unbiased_exp;
        logic signed [10:0] rhs_unbiased_exp;
        logic signed [10:0] result_unbiased_exp;
        logic [23:0] lhs_sig;
        logic [23:0] rhs_sig;
        logic [63:0] dividend;
        logic [63:0] quotient;
        logic [23:0] remainder;
        logic [27:0] result_sig;
        int lhs_msb;
        int rhs_msb;

        flags = '0;
        result_sign = lhs[31] ^ rhs[31];
        lhs_exp = lhs[30:23];
        rhs_exp = rhs[30:23];
        lhs_frac = lhs[22:0];
        rhs_frac = rhs[22:0];
        lhs_is_zero = (lhs[30:0] == '0);
        rhs_is_zero = (rhs[30:0] == '0);
        lhs_is_inf = (&lhs_exp) && (lhs_frac == '0);
        rhs_is_inf = (&rhs_exp) && (rhs_frac == '0);

        if (is_nan_s(lhs) || is_nan_s(rhs)) begin
            flags[4] = is_snan_s(lhs) || is_snan_s(rhs);
            return CANONICAL_NAN_S;
        end

        if ((lhs_is_zero && rhs_is_zero) || (lhs_is_inf && rhs_is_inf)) begin
            flags[4] = 1'b1;
            return CANONICAL_NAN_S;
        end

        if (lhs_is_inf || rhs_is_zero) begin
            if (rhs_is_zero && !lhs_is_zero && !lhs_is_inf) begin
                flags[3] = 1'b1;
            end
            return {result_sign, 8'hFF, 23'b0};
        end

        if (lhs_is_zero || rhs_is_inf) begin
            return {result_sign, 31'b0};
        end

        if (lhs_exp == '0) begin
            lhs_msb = highest_bit_64({41'b0, lhs_frac});
            lhs_unbiased_exp = 11'(lhs_msb - 149);
            lhs_sig = {1'b0, lhs_frac} << (23 - lhs_msb);
        end else begin
            lhs_unbiased_exp = $signed({3'b0, lhs_exp}) - 11'sd127;
            lhs_sig = {1'b1, lhs_frac};
        end

        if (rhs_exp == '0) begin
            rhs_msb = highest_bit_64({41'b0, rhs_frac});
            rhs_unbiased_exp = 11'(rhs_msb - 149);
            rhs_sig = {1'b0, rhs_frac} << (23 - rhs_msb);
        end else begin
            rhs_unbiased_exp = $signed({3'b0, rhs_exp}) - 11'sd127;
            rhs_sig = {1'b1, rhs_frac};
        end

        if (lhs_sig >= rhs_sig) begin
            result_unbiased_exp = lhs_unbiased_exp - rhs_unbiased_exp;
            dividend = {40'b0, lhs_sig} << 26;
        end else begin
            result_unbiased_exp = lhs_unbiased_exp - rhs_unbiased_exp - 11'sd1;
            dividend = {40'b0, lhs_sig} << 27;
        end

        quotient = dividend / {40'b0, rhs_sig};
        remainder = 24'(dividend % {40'b0, rhs_sig});
        result_sig = 28'(quotient);
        if (remainder != '0) begin
            result_sig[0] = 1'b1;
        end

        return round_pack_s(result_sign, result_unbiased_exp, result_sig, 1'b0, round_mode, flags);
    endfunction

    function automatic logic [63:0] fp_div_d(
        input logic [63:0] lhs,
        input logic [63:0] rhs,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic result_sign;
        logic [10:0] lhs_exp;
        logic [10:0] rhs_exp;
        logic [51:0] lhs_frac;
        logic [51:0] rhs_frac;
        logic lhs_is_zero;
        logic rhs_is_zero;
        logic lhs_is_inf;
        logic rhs_is_inf;
        logic signed [12:0] lhs_unbiased_exp;
        logic signed [12:0] rhs_unbiased_exp;
        logic signed [12:0] result_unbiased_exp;
        logic [52:0] lhs_sig;
        logic [52:0] rhs_sig;
        logic [127:0] dividend;
        logic [127:0] quotient;
        logic [52:0] remainder;
        logic [56:0] result_sig;
        int lhs_msb;
        int rhs_msb;

        flags = '0;
        result_sign = lhs[63] ^ rhs[63];
        lhs_exp = lhs[62:52];
        rhs_exp = rhs[62:52];
        lhs_frac = lhs[51:0];
        rhs_frac = rhs[51:0];
        lhs_is_zero = (lhs[62:0] == '0);
        rhs_is_zero = (rhs[62:0] == '0);
        lhs_is_inf = (&lhs_exp) && (lhs_frac == '0);
        rhs_is_inf = (&rhs_exp) && (rhs_frac == '0);

        if (is_nan_d(lhs) || is_nan_d(rhs)) begin
            flags[4] = is_snan_d(lhs) || is_snan_d(rhs);
            return CANONICAL_NAN_D;
        end

        if ((lhs_is_zero && rhs_is_zero) || (lhs_is_inf && rhs_is_inf)) begin
            flags[4] = 1'b1;
            return CANONICAL_NAN_D;
        end

        if (lhs_is_inf || rhs_is_zero) begin
            if (rhs_is_zero && !lhs_is_zero && !lhs_is_inf) begin
                flags[3] = 1'b1;
            end
            return {result_sign, 11'h7FF, 52'b0};
        end

        if (lhs_is_zero || rhs_is_inf) begin
            return {result_sign, 63'b0};
        end

        if (lhs_exp == '0) begin
            lhs_msb = highest_bit_53({1'b0, lhs_frac});
            lhs_unbiased_exp = 13'(lhs_msb - 1074);
            lhs_sig = {1'b0, lhs_frac} << (52 - lhs_msb);
        end else begin
            lhs_unbiased_exp = $signed({2'b0, lhs_exp}) - 13'sd1023;
            lhs_sig = {1'b1, lhs_frac};
        end

        if (rhs_exp == '0) begin
            rhs_msb = highest_bit_53({1'b0, rhs_frac});
            rhs_unbiased_exp = 13'(rhs_msb - 1074);
            rhs_sig = {1'b0, rhs_frac} << (52 - rhs_msb);
        end else begin
            rhs_unbiased_exp = $signed({2'b0, rhs_exp}) - 13'sd1023;
            rhs_sig = {1'b1, rhs_frac};
        end

        if (lhs_sig >= rhs_sig) begin
            result_unbiased_exp = lhs_unbiased_exp - rhs_unbiased_exp;
            dividend = {75'b0, lhs_sig} << 55;
        end else begin
            result_unbiased_exp = lhs_unbiased_exp - rhs_unbiased_exp - 13'sd1;
            dividend = {75'b0, lhs_sig} << 56;
        end

        quotient = dividend / {75'b0, rhs_sig};
        remainder = 53'(dividend % {75'b0, rhs_sig});
        result_sig = 57'(quotient);
        if (remainder != '0) begin
            result_sig[0] = 1'b1;
        end

        return round_pack_d(result_sign, result_unbiased_exp, result_sig, 1'b0, round_mode, flags);
    endfunction

    function automatic logic [31:0] fp_sqrt_s(
        input logic [31:0] value,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic sign;
        logic [7:0] exp;
        logic [22:0] frac;
        logic signed [10:0] unbiased_exp;
        logic signed [10:0] result_unbiased_exp;
        logic [23:0] sig;
        logic [63:0] radicand;
        logic [63:0] trial;
        logic [63:0] root;
        logic [63:0] remainder;
        logic [27:0] result_sig;
        int sig_msb;

        flags = '0;
        sign = value[31];
        exp = value[30:23];
        frac = value[22:0];

        if (is_nan_s(value)) begin
            flags[4] = is_snan_s(value);
            return CANONICAL_NAN_S;
        end

        if ((&exp) && (frac == '0)) begin
            if (sign) begin
                flags[4] = 1'b1;
                return CANONICAL_NAN_S;
            end
            return value;
        end

        if (value[30:0] == '0) begin
            return value;
        end

        if (sign) begin
            flags[4] = 1'b1;
            return CANONICAL_NAN_S;
        end

        if (exp == '0) begin
            sig_msb = highest_bit_64({41'b0, frac});
            unbiased_exp = 11'(sig_msb - 149);
            sig = {1'b0, frac} << (23 - sig_msb);
        end else begin
            unbiased_exp = $signed({3'b0, exp}) - 11'sd127;
            sig = {1'b1, frac};
        end

        if (unbiased_exp[0]) begin
            radicand = {39'b0, sig, 1'b0} << 29;
            result_unbiased_exp = (unbiased_exp - 11'sd1) >>> 1;
        end else begin
            radicand = {40'b0, sig} << 29;
            result_unbiased_exp = unbiased_exp >>> 1;
        end

        root = '0;
        for (int bit_idx = 26; bit_idx >= 0; bit_idx--) begin
            trial = root | (64'd1 << bit_idx);
            if ((trial * trial) <= radicand) begin
                root = trial;
            end
        end

        remainder = radicand - (root * root);
        result_sig = 28'(root);
        if (remainder != '0) begin
            result_sig[0] = 1'b1;
        end

        return round_pack_s(1'b0, result_unbiased_exp, result_sig, 1'b0, round_mode, flags);
    endfunction

    function automatic logic [63:0] fp_sqrt_d(
        input logic [63:0] value,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic sign;
        logic [10:0] exp;
        logic [51:0] frac;
        logic signed [12:0] unbiased_exp;
        logic signed [12:0] result_unbiased_exp;
        logic [52:0] sig;
        logic [127:0] radicand;
        logic [127:0] trial;
        logic [127:0] root;
        logic [127:0] remainder;
        logic [56:0] result_sig;
        int sig_msb;

        flags = '0;
        sign = value[63];
        exp = value[62:52];
        frac = value[51:0];

        if (is_nan_d(value)) begin
            flags[4] = is_snan_d(value);
            return CANONICAL_NAN_D;
        end

        if ((&exp) && (frac == '0)) begin
            if (sign) begin
                flags[4] = 1'b1;
                return CANONICAL_NAN_D;
            end
            return value;
        end

        if (value[62:0] == '0) begin
            return value;
        end

        if (sign) begin
            flags[4] = 1'b1;
            return CANONICAL_NAN_D;
        end

        if (exp == '0) begin
            sig_msb = highest_bit_53({1'b0, frac});
            unbiased_exp = 13'(sig_msb - 1074);
            sig = {1'b0, frac} << (52 - sig_msb);
        end else begin
            unbiased_exp = $signed({2'b0, exp}) - 13'sd1023;
            sig = {1'b1, frac};
        end

        if (unbiased_exp[0]) begin
            radicand = {74'b0, sig, 1'b0} << 58;
            result_unbiased_exp = (unbiased_exp - 13'sd1) >>> 1;
        end else begin
            radicand = {75'b0, sig} << 58;
            result_unbiased_exp = unbiased_exp >>> 1;
        end

        root = '0;
        for (int bit_idx = 55; bit_idx >= 0; bit_idx--) begin
            trial = root | (128'd1 << bit_idx);
            if ((trial * trial) <= radicand) begin
                root = trial;
            end
        end

        remainder = radicand - (root * root);
        result_sig = 57'(root);
        if (remainder != '0) begin
            result_sig[0] = 1'b1;
        end

        return round_pack_d(1'b0, result_unbiased_exp, result_sig, 1'b0, round_mode, flags);
    endfunction

    function automatic logic [31:0] fp_fma_s(
        input logic [31:0] mul_lhs,
        input logic [31:0] mul_rhs,
        input logic [31:0] addend,
        input logic        negate_product,
        input logic        negate_addend,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic lhs_sign;
        logic rhs_sign;
        logic addend_sign;
        logic product_sign;
        logic result_sign;
        logic [7:0] lhs_exp;
        logic [7:0] rhs_exp;
        logic [7:0] addend_exp;
        logic [22:0] lhs_frac;
        logic [22:0] rhs_frac;
        logic [22:0] addend_frac;
        logic lhs_is_zero;
        logic rhs_is_zero;
        logic addend_is_zero;
        logic lhs_is_inf;
        logic rhs_is_inf;
        logic addend_is_inf;
        logic invalid_multiply;
        logic signed [10:0] lhs_unbiased_exp;
        logic signed [10:0] rhs_unbiased_exp;
        logic signed [10:0] addend_unbiased_exp;
        logic signed [10:0] product_unbiased_exp;
        logic signed [10:0] common_unbiased_exp;
        logic signed [10:0] result_unbiased_exp;
        logic [23:0] lhs_sig;
        logic [23:0] rhs_sig;
        logic [23:0] addend_sig;
        logic [47:0] product;
        logic [47:0] normalized_product;
        logic [64:0] product_ext;
        logic [64:0] addend_ext;
        logic [64:0] aligned_product;
        logic [64:0] aligned_addend;
        logic [64:0] result_ext;
        logic [64:0] rounded_source;
        logic [27:0] result_sig;
        int lhs_msb;
        int rhs_msb;
        int addend_msb;
        int exp_diff;

        flags = '0;
        lhs_sign = mul_lhs[31];
        rhs_sign = mul_rhs[31];
        addend_sign = addend[31] ^ negate_addend;
        product_sign = lhs_sign ^ rhs_sign ^ negate_product;
        lhs_exp = mul_lhs[30:23];
        rhs_exp = mul_rhs[30:23];
        addend_exp = addend[30:23];
        lhs_frac = mul_lhs[22:0];
        rhs_frac = mul_rhs[22:0];
        addend_frac = addend[22:0];
        lhs_is_zero = (mul_lhs[30:0] == '0);
        rhs_is_zero = (mul_rhs[30:0] == '0);
        addend_is_zero = (addend[30:0] == '0);
        lhs_is_inf = (&lhs_exp) && (lhs_frac == '0);
        rhs_is_inf = (&rhs_exp) && (rhs_frac == '0);
        addend_is_inf = (&addend_exp) && (addend_frac == '0);
        invalid_multiply = (lhs_is_inf && rhs_is_zero) || (rhs_is_inf && lhs_is_zero);

        if (is_nan_s(mul_lhs) || is_nan_s(mul_rhs) || is_nan_s(addend) || invalid_multiply) begin
            flags[4] = invalid_multiply || is_snan_s(mul_lhs) || is_snan_s(mul_rhs) || is_snan_s(addend);
            return CANONICAL_NAN_S;
        end

        if (lhs_is_inf || rhs_is_inf) begin
            if (addend_is_inf && (product_sign != addend_sign)) begin
                flags[4] = 1'b1;
                return CANONICAL_NAN_S;
            end
            return {product_sign, 8'hFF, 23'b0};
        end

        if (addend_is_inf) begin
            return {addend_sign, 8'hFF, 23'b0};
        end

        if (lhs_is_zero || rhs_is_zero) begin
            if (addend_is_zero) begin
                result_sign = (product_sign == addend_sign) ? product_sign : (effective_rm(round_mode) == 3'b010);
                return {result_sign, 31'b0};
            end
            return {addend_sign, addend[30:0]};
        end

        if (addend_is_zero) begin
            return fp_mul_s({mul_lhs[31] ^ negate_product, mul_lhs[30:0]}, mul_rhs, round_mode, flags);
        end

        if (lhs_exp == '0) begin
            lhs_msb = highest_bit_64({41'b0, lhs_frac});
            lhs_unbiased_exp = 11'(lhs_msb - 149);
            lhs_sig = {1'b0, lhs_frac} << (23 - lhs_msb);
        end else begin
            lhs_unbiased_exp = $signed({3'b0, lhs_exp}) - 11'sd127;
            lhs_sig = {1'b1, lhs_frac};
        end

        if (rhs_exp == '0) begin
            rhs_msb = highest_bit_64({41'b0, rhs_frac});
            rhs_unbiased_exp = 11'(rhs_msb - 149);
            rhs_sig = {1'b0, rhs_frac} << (23 - rhs_msb);
        end else begin
            rhs_unbiased_exp = $signed({3'b0, rhs_exp}) - 11'sd127;
            rhs_sig = {1'b1, rhs_frac};
        end

        if (addend_exp == '0) begin
            addend_msb = highest_bit_64({41'b0, addend_frac});
            addend_unbiased_exp = 11'(addend_msb - 149);
            addend_sig = {1'b0, addend_frac} << (23 - addend_msb);
        end else begin
            addend_unbiased_exp = $signed({3'b0, addend_exp}) - 11'sd127;
            addend_sig = {1'b1, addend_frac};
        end

        product = lhs_sig * rhs_sig;
        if (product[47]) begin
            product_unbiased_exp = lhs_unbiased_exp + rhs_unbiased_exp + 11'sd1;
            normalized_product = product;
        end else begin
            product_unbiased_exp = lhs_unbiased_exp + rhs_unbiased_exp;
            normalized_product = product << 1;
        end

        product_ext = {4'b0, normalized_product, 13'b0};
        addend_ext = {4'b0, addend_sig, 37'b0};

        if (product_unbiased_exp >= addend_unbiased_exp) begin
            common_unbiased_exp = product_unbiased_exp;
            exp_diff = int'(product_unbiased_exp) - int'(addend_unbiased_exp);
            aligned_product = product_ext;
            aligned_addend = shift_right_jam_65(addend_ext, exp_diff);
        end else begin
            common_unbiased_exp = addend_unbiased_exp;
            exp_diff = int'(addend_unbiased_exp) - int'(product_unbiased_exp);
            aligned_product = shift_right_jam_65(product_ext, exp_diff);
            aligned_addend = addend_ext;
        end

        result_unbiased_exp = common_unbiased_exp;
        if (product_sign == addend_sign) begin
            result_ext = aligned_product + aligned_addend;
            result_sign = product_sign;
            if (result_ext[61]) begin
                result_ext = shift_right_jam_65(result_ext, 1);
                result_unbiased_exp = result_unbiased_exp + 11'sd1;
            end
        end else if (aligned_product >= aligned_addend) begin
            result_ext = aligned_product - aligned_addend;
            result_sign = product_sign;
        end else begin
            result_ext = aligned_addend - aligned_product;
            result_sign = addend_sign;
        end

        if (result_ext == '0) begin
            result_sign = effective_rm(round_mode) == 3'b010;
            return {result_sign, 31'b0};
        end

        while (!result_ext[60] && (result_unbiased_exp > -11'sd126)) begin
            result_ext = result_ext << 1;
            result_unbiased_exp = result_unbiased_exp - 11'sd1;
        end

        rounded_source = shift_right_jam_65(result_ext, 34);
        result_sig = rounded_source[27:0];
        return round_pack_s(result_sign, result_unbiased_exp, result_sig, 1'b0, round_mode, flags);
    endfunction

    function automatic logic [63:0] fp_fma_d(
        input logic [63:0] mul_lhs,
        input logic [63:0] mul_rhs,
        input logic [63:0] addend,
        input logic        negate_product,
        input logic        negate_addend,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic lhs_sign;
        logic rhs_sign;
        logic addend_sign;
        logic product_sign;
        logic result_sign;
        logic [10:0] lhs_exp;
        logic [10:0] rhs_exp;
        logic [10:0] addend_exp;
        logic [51:0] lhs_frac;
        logic [51:0] rhs_frac;
        logic [51:0] addend_frac;
        logic lhs_is_zero;
        logic rhs_is_zero;
        logic addend_is_zero;
        logic lhs_is_inf;
        logic rhs_is_inf;
        logic addend_is_inf;
        logic invalid_multiply;
        logic signed [12:0] lhs_unbiased_exp;
        logic signed [12:0] rhs_unbiased_exp;
        logic signed [12:0] addend_unbiased_exp;
        logic signed [12:0] product_unbiased_exp;
        logic signed [12:0] common_unbiased_exp;
        logic signed [12:0] result_unbiased_exp;
        logic [52:0] lhs_sig;
        logic [52:0] rhs_sig;
        logic [52:0] addend_sig;
        logic [105:0] product;
        logic [105:0] normalized_product;
        logic [127:0] product_ext;
        logic [127:0] addend_ext;
        logic [127:0] aligned_product;
        logic [127:0] aligned_addend;
        logic [127:0] result_ext;
        logic [127:0] rounded_source;
        logic [56:0] result_sig;
        int lhs_msb;
        int rhs_msb;
        int addend_msb;
        int exp_diff;

        flags = '0;
        lhs_sign = mul_lhs[63];
        rhs_sign = mul_rhs[63];
        addend_sign = addend[63] ^ negate_addend;
        product_sign = lhs_sign ^ rhs_sign ^ negate_product;
        lhs_exp = mul_lhs[62:52];
        rhs_exp = mul_rhs[62:52];
        addend_exp = addend[62:52];
        lhs_frac = mul_lhs[51:0];
        rhs_frac = mul_rhs[51:0];
        addend_frac = addend[51:0];
        lhs_is_zero = (mul_lhs[62:0] == '0);
        rhs_is_zero = (mul_rhs[62:0] == '0);
        addend_is_zero = (addend[62:0] == '0);
        lhs_is_inf = (&lhs_exp) && (lhs_frac == '0);
        rhs_is_inf = (&rhs_exp) && (rhs_frac == '0);
        addend_is_inf = (&addend_exp) && (addend_frac == '0);
        invalid_multiply = (lhs_is_inf && rhs_is_zero) || (rhs_is_inf && lhs_is_zero);

        if (is_nan_d(mul_lhs) || is_nan_d(mul_rhs) || is_nan_d(addend) || invalid_multiply) begin
            flags[4] = invalid_multiply || is_snan_d(mul_lhs) || is_snan_d(mul_rhs) || is_snan_d(addend);
            return CANONICAL_NAN_D;
        end

        if (lhs_is_inf || rhs_is_inf) begin
            if (addend_is_inf && (product_sign != addend_sign)) begin
                flags[4] = 1'b1;
                return CANONICAL_NAN_D;
            end
            return {product_sign, 11'h7FF, 52'b0};
        end

        if (addend_is_inf) begin
            return {addend_sign, 11'h7FF, 52'b0};
        end

        if (lhs_is_zero || rhs_is_zero) begin
            if (addend_is_zero) begin
                result_sign = (product_sign == addend_sign) ? product_sign : (effective_rm(round_mode) == 3'b010);
                return {result_sign, 63'b0};
            end
            return {addend_sign, addend[62:0]};
        end

        if (addend_is_zero) begin
            return fp_mul_d({mul_lhs[63] ^ negate_product, mul_lhs[62:0]}, mul_rhs, round_mode, flags);
        end

        if (lhs_exp == '0) begin
            lhs_msb = highest_bit_53({1'b0, lhs_frac});
            lhs_unbiased_exp = 13'(lhs_msb - 1074);
            lhs_sig = {1'b0, lhs_frac} << (52 - lhs_msb);
        end else begin
            lhs_unbiased_exp = $signed({2'b0, lhs_exp}) - 13'sd1023;
            lhs_sig = {1'b1, lhs_frac};
        end

        if (rhs_exp == '0) begin
            rhs_msb = highest_bit_53({1'b0, rhs_frac});
            rhs_unbiased_exp = 13'(rhs_msb - 1074);
            rhs_sig = {1'b0, rhs_frac} << (52 - rhs_msb);
        end else begin
            rhs_unbiased_exp = $signed({2'b0, rhs_exp}) - 13'sd1023;
            rhs_sig = {1'b1, rhs_frac};
        end

        if (addend_exp == '0) begin
            addend_msb = highest_bit_53({1'b0, addend_frac});
            addend_unbiased_exp = 13'(addend_msb - 1074);
            addend_sig = {1'b0, addend_frac} << (52 - addend_msb);
        end else begin
            addend_unbiased_exp = $signed({2'b0, addend_exp}) - 13'sd1023;
            addend_sig = {1'b1, addend_frac};
        end

        product = lhs_sig * rhs_sig;
        if (product[105]) begin
            product_unbiased_exp = lhs_unbiased_exp + rhs_unbiased_exp + 13'sd1;
            normalized_product = product;
        end else begin
            product_unbiased_exp = lhs_unbiased_exp + rhs_unbiased_exp;
            normalized_product = product << 1;
        end

        product_ext = {9'b0, normalized_product, 13'b0};
        addend_ext = {9'b0, addend_sig, 66'b0};

        if (product_unbiased_exp >= addend_unbiased_exp) begin
            common_unbiased_exp = product_unbiased_exp;
            exp_diff = int'(product_unbiased_exp) - int'(addend_unbiased_exp);
            aligned_product = product_ext;
            aligned_addend = shift_right_jam_128(addend_ext, exp_diff);
        end else begin
            common_unbiased_exp = addend_unbiased_exp;
            exp_diff = int'(addend_unbiased_exp) - int'(product_unbiased_exp);
            aligned_product = shift_right_jam_128(product_ext, exp_diff);
            aligned_addend = addend_ext;
        end

        result_unbiased_exp = common_unbiased_exp;
        if (product_sign == addend_sign) begin
            result_ext = aligned_product + aligned_addend;
            result_sign = product_sign;
            if (result_ext[119]) begin
                result_ext = shift_right_jam_128(result_ext, 1);
                result_unbiased_exp = result_unbiased_exp + 13'sd1;
            end
        end else if (aligned_product >= aligned_addend) begin
            result_ext = aligned_product - aligned_addend;
            result_sign = product_sign;
        end else begin
            result_ext = aligned_addend - aligned_product;
            result_sign = addend_sign;
        end

        if (result_ext == '0) begin
            result_sign = effective_rm(round_mode) == 3'b010;
            return {result_sign, 63'b0};
        end

        while (!result_ext[118] && (result_unbiased_exp > -13'sd1022)) begin
            result_ext = result_ext << 1;
            result_unbiased_exp = result_unbiased_exp - 13'sd1;
        end

        rounded_source = shift_right_jam_128(result_ext, 63);
        result_sig = rounded_source[56:0];
        return round_pack_d(result_sign, result_unbiased_exp, result_sig, 1'b0, round_mode, flags);
    endfunction

    function automatic logic [63:0] pack_uint_to_s(
        input logic        sign,
        input logic [63:0] magnitude,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        int msb_index;
        int shift;
        logic [127:0] quotient;
        logic [127:0] remainder;
        logic [127:0] divisor;
        logic [127:0] half;
        logic [24:0] rounded_sig;
        logic [7:0] exp_field;
        logic [31:0] result_s;

        flags = '0;
        if (magnitude == '0) begin
            return {32'hFFFF_FFFF, sign, 31'b0};
        end

        msb_index = highest_bit_64(magnitude);
        exp_field = 8'(msb_index + 127);
        if (msb_index <= 23) begin
            rounded_sig = {1'b0, magnitude[23:0] << (23 - msb_index)};
        end else begin
            shift     = msb_index - 23;
            divisor   = 128'd1 << shift;
            quotient  = {64'b0, magnitude} >> shift;
            remainder = {64'b0, magnitude} & (divisor - 128'd1);
            half      = divisor >> 1;
            rounded_sig = {1'b0, quotient[23:0]} +
                          (round_increment(round_mode, sign, quotient, remainder, half) ? 25'd1 : 25'd0);
            if (remainder != '0) begin
                flags[0] = 1'b1;
            end
            if (rounded_sig[24]) begin
                rounded_sig = rounded_sig >> 1;
                exp_field = exp_field + 8'd1;
            end
        end

        result_s = {sign, exp_field, rounded_sig[22:0]};
        return {32'hFFFF_FFFF, result_s};
    endfunction

    function automatic logic [63:0] pack_uint_to_d(
        input logic        sign,
        input logic [63:0] magnitude,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        int msb_index;
        int shift;
        logic [127:0] quotient;
        logic [127:0] remainder;
        logic [127:0] divisor;
        logic [127:0] half;
        logic [53:0] rounded_sig;
        logic [10:0] exp_field;

        flags = '0;
        if (magnitude == '0) begin
            return {sign, 63'b0};
        end

        msb_index = highest_bit_64(magnitude);
        exp_field = 11'(msb_index + 1023);
        if (msb_index <= 52) begin
            rounded_sig = {1'b0, magnitude[52:0] << (52 - msb_index)};
        end else begin
            shift     = msb_index - 52;
            divisor   = 128'd1 << shift;
            quotient  = {64'b0, magnitude} >> shift;
            remainder = {64'b0, magnitude} & (divisor - 128'd1);
            half      = divisor >> 1;
            rounded_sig = {1'b0, quotient[52:0]} +
                          (round_increment(round_mode, sign, quotient, remainder, half) ? 54'd1 : 54'd0);
            if (remainder != '0) begin
                flags[0] = 1'b1;
            end
            if (rounded_sig[53]) begin
                rounded_sig = rounded_sig >> 1;
                exp_field = exp_field + 11'd1;
            end
        end

        return {sign, exp_field, rounded_sig[51:0]};
    endfunction

    function automatic logic [63:0] cvt_int_to_fp(
        input logic [63:0] int_value,
        input logic [1:0]  int_fmt,
        input logic        to_double,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic is_signed;
        logic is_long;
        logic sign;
        logic [63:0] source_value;
        logic [63:0] magnitude;

        is_signed    = (int_fmt == 2'd0) || (int_fmt == 2'd2);
        is_long      = (int_fmt == 2'd2) || (int_fmt == 2'd3);
        source_value = is_long ? int_value :
                       (is_signed ? {{32{int_value[31]}}, int_value[31:0]} : {32'b0, int_value[31:0]});
        sign         = is_signed && source_value[63];
        magnitude    = sign ? ((~source_value) + 64'd1) : source_value;

        if (to_double) begin
            return pack_uint_to_d(sign, magnitude, round_mode, flags);
        end

        return pack_uint_to_s(sign, magnitude, round_mode, flags);
    endfunction

    function automatic logic [63:0] cvt_s_to_d(
        input logic [31:0] value,
        output logic [4:0] flags
    );
        logic sign;
        logic [7:0] exp;
        logic [22:0] frac;
        int frac_msb;
        logic [10:0] exp_d;
        logic [51:0] frac_d;
        logic [22:0] subnormal_tail_s;

        flags = '0;
        sign  = value[31];
        exp   = value[30:23];
        frac  = value[22:0];

        if (is_nan_s(value)) begin
            flags[4] = is_snan_s(value);
            return CANONICAL_NAN_D;
        end
        if (&exp) begin
            return {sign, 11'h7FF, 52'b0};
        end
        if ((exp == '0) && (frac == '0)) begin
            return {sign, 63'b0};
        end
        if (exp == '0) begin
            frac_msb = -1;
            for (int bit_idx = 0; bit_idx < 23; bit_idx++) begin
                if (frac[bit_idx]) begin
                    frac_msb = bit_idx;
                end
            end
            exp_d = 11'(frac_msb - 149 + 1023);
            subnormal_tail_s = frac ^ (23'(1) << frac_msb);
            frac_d = {29'b0, subnormal_tail_s} << (52 - frac_msb);
            return {sign, exp_d, frac_d};
        end

        exp_d = 11'(int'(exp) - 127 + 1023);
        frac_d = {frac, 29'b0};
        return {sign, exp_d, frac_d};
    endfunction

    function automatic logic [63:0] cvt_d_to_s(
        input logic [63:0] value,
        input logic [2:0]  round_mode,
        output logic [4:0] flags
    );
        logic sign;
        logic [10:0] exp;
        logic [51:0] frac;
        logic [52:0] significand;
        logic signed [13:0] unbiased_exp;
        int frac_msb;
        int shift;
        logic [127:0] quotient;
        logic [127:0] remainder;
        logic [127:0] divisor;
        logic [127:0] half;
        logic [24:0] rounded_sig;
        logic [7:0] exp_s;
        logic [31:0] result_s;

        flags = '0;
        sign  = value[63];
        exp   = value[62:52];
        frac  = value[51:0];

        if (is_nan_d(value)) begin
            flags[4] = is_snan_d(value);
            return {32'hFFFF_FFFF, CANONICAL_NAN_S};
        end
        if (&exp) begin
            return {32'hFFFF_FFFF, sign, 8'hFF, 23'b0};
        end
        if ((exp == '0) && (frac == '0)) begin
            return {32'hFFFF_FFFF, sign, 31'b0};
        end

        if (exp == '0) begin
            frac_msb = highest_bit_53({1'b0, frac});
            unbiased_exp = 14'(frac_msb - 1074);
            significand = {1'b0, frac} << (52 - frac_msb);
        end else begin
            unbiased_exp = $signed({3'b0, exp}) - 14'sd1023;
            significand = {1'b1, frac};
        end

        if (unbiased_exp > 14'sd127) begin
            flags[2] = 1'b1;
            flags[0] = 1'b1;
            return {32'hFFFF_FFFF, overflow_s_result(sign, round_mode)};
        end

        if (unbiased_exp >= -14'sd126) begin
            exp_s = 8'(unbiased_exp + 14'sd127);
            shift = 29;
            divisor = 128'd1 << shift;
            quotient = {75'b0, significand} >> shift;
            remainder = {75'b0, significand} & (divisor - 128'd1);
            half = divisor >> 1;
            rounded_sig = {1'b0, quotient[23:0]} +
                          (round_increment(round_mode, sign, quotient, remainder, half) ? 25'd1 : 25'd0);
            if (remainder != '0) begin
                flags[0] = 1'b1;
            end
            if (rounded_sig[24]) begin
                rounded_sig = rounded_sig >> 1;
                exp_s = exp_s + 8'd1;
                if (exp_s == 8'hFF) begin
                    flags[2] = 1'b1;
                    flags[0] = 1'b1;
                    return {32'hFFFF_FFFF, overflow_s_result(sign, round_mode)};
                end
            end
            result_s = {sign, exp_s, rounded_sig[22:0]};
            return {32'hFFFF_FFFF, result_s};
        end

        shift = 29 + ((-126) - int'(unbiased_exp));
        if (shift >= 128) begin
            quotient = '0;
            remainder = {75'b0, significand};
            rounded_sig = '0;
            if (remainder != '0) begin
                flags[1] = 1'b1;
                flags[0] = 1'b1;
                unique case (effective_rm(round_mode))
                    3'b010:  rounded_sig = sign ? 25'd1 : 25'd0;
                    3'b011:  rounded_sig = !sign ? 25'd1 : 25'd0;
                    default: rounded_sig = '0;
                endcase
            end
        end else begin
            divisor = 128'd1 << shift;
            quotient = {75'b0, significand} >> shift;
            remainder = {75'b0, significand} & (divisor - 128'd1);
            half = divisor >> 1;
            rounded_sig = {1'b0, quotient[23:0]} +
                          (round_increment(round_mode, sign, quotient, remainder, half) ? 25'd1 : 25'd0);
            if (remainder != '0) begin
                flags[1] = 1'b1;
                flags[0] = 1'b1;
            end
        end

        if (rounded_sig[23]) begin
            result_s = {sign, 8'd1, 23'b0};
        end else begin
            result_s = {sign, 8'b0, rounded_sig[22:0]};
        end
        return {32'hFFFF_FFFF, result_s};
    endfunction

    always_comb begin
        logic invalid_s;
        logic invalid_d;
        logic cmp_value;
        logic [31:0] result_s;
        logic [63:0] result_d;
        logic [63:0] result_int;
        logic [4:0] result_flags;

        fp_result     = '0;
        int_result    = '0;
        fflags        = '0;
        illegal_instr = 1'b0;
        invalid_s     = is_nan_s(s1) || is_nan_s(s2);
        invalid_d     = is_nan_d(d1) || is_nan_d(d2);
        cmp_value     = 1'b0;
        result_s      = '0;
        result_d      = '0;
        result_int    = '0;
        result_flags  = '0;

        unique case (fp_op)
            FP_OP_NONE: begin
                fp_result  = '0;
                int_result = '0;
                fflags     = '0;
            end

            FP_OP_ADD, FP_OP_SUB: begin
                if (fp_fmt == FP_FMT_S) begin
                    result_s = fp_addsub_s(s1, s2, fp_op == FP_OP_SUB, rm, result_flags);
                    fp_result = {32'hFFFF_FFFF, result_s};
                    fflags = result_flags;
                end else begin
                    result_d = fp_addsub_d(d1, d2, fp_op == FP_OP_SUB, rm, result_flags);
                    fp_result = result_d;
                    fflags = result_flags;
                end
            end

            FP_OP_MUL: begin
                if (fp_fmt == FP_FMT_S) begin
                    result_s = fp_mul_s(s1, s2, rm, result_flags);
                    fp_result = {32'hFFFF_FFFF, result_s};
                    fflags = result_flags;
                end else begin
                    result_d = fp_mul_d(d1, d2, rm, result_flags);
                    fp_result = result_d;
                    fflags = result_flags;
                end
            end

            FP_OP_DIV: begin
                if (fp_fmt == FP_FMT_S) begin
                    result_s = fp_div_s(s1, s2, rm, result_flags);
                    fp_result = {32'hFFFF_FFFF, result_s};
                    fflags = result_flags;
                end else begin
                    result_d = fp_div_d(d1, d2, rm, result_flags);
                    fp_result = result_d;
                    fflags = result_flags;
                end
            end

            FP_OP_SQRT: begin
                if (fp_fmt == FP_FMT_S) begin
                    result_s = fp_sqrt_s(s1, rm, result_flags);
                    fp_result = {32'hFFFF_FFFF, result_s};
                    fflags = result_flags;
                end else begin
                    result_d = fp_sqrt_d(d1, rm, result_flags);
                    fp_result = result_d;
                    fflags = result_flags;
                end
            end

            FP_OP_MADD, FP_OP_MSUB, FP_OP_NMSUB, FP_OP_NMADD: begin
                if (fp_fmt == FP_FMT_S) begin
                    unique case (fp_op)
                        FP_OP_MADD:  result_s = fp_fma_s(s1, s2, s3, 1'b0, 1'b0, rm, result_flags);
                        FP_OP_MSUB:  result_s = fp_fma_s(s1, s2, s3, 1'b0, 1'b1, rm, result_flags);
                        FP_OP_NMSUB: result_s = fp_fma_s(s1, s2, s3, 1'b1, 1'b0, rm, result_flags);
                        default:     result_s = fp_fma_s(s1, s2, s3, 1'b1, 1'b1, rm, result_flags);
                    endcase
                    fp_result = {32'hFFFF_FFFF, result_s};
                    fflags = result_flags;
                end else begin
                    unique case (fp_op)
                        FP_OP_MADD:  result_d = fp_fma_d(d1, d2, d3, 1'b0, 1'b0, rm, result_flags);
                        FP_OP_MSUB:  result_d = fp_fma_d(d1, d2, d3, 1'b0, 1'b1, rm, result_flags);
                        FP_OP_NMSUB: result_d = fp_fma_d(d1, d2, d3, 1'b1, 1'b0, rm, result_flags);
                        default:     result_d = fp_fma_d(d1, d2, d3, 1'b1, 1'b1, rm, result_flags);
                    endcase
                    fp_result = result_d;
                    fflags = result_flags;
                end
            end

            FP_OP_SGNJ, FP_OP_SGNJN, FP_OP_SGNJX: begin
                if (fp_fmt == FP_FMT_S) begin
                    unique case (fp_op)
                        FP_OP_SGNJ:  fp_result = {32'hFFFF_FFFF, s2[31],  s1[30:0]};
                        FP_OP_SGNJN: fp_result = {32'hFFFF_FFFF, ~s2[31], s1[30:0]};
                        default:     fp_result = {32'hFFFF_FFFF, s1[31] ^ s2[31], s1[30:0]};
                    endcase
                end else begin
                    unique case (fp_op)
                        FP_OP_SGNJ:  fp_result = {d2[63],  d1[62:0]};
                        FP_OP_SGNJN: fp_result = {~d2[63], d1[62:0]};
                        default:     fp_result = {d1[63] ^ d2[63], d1[62:0]};
                    endcase
                end
            end

            FP_OP_MV_X2F: begin
                fp_result = (fp_fmt == FP_FMT_S) ? {32'hFFFF_FFFF, int_rs1_data[31:0]} : int_rs1_data;
            end

            FP_OP_MV_F2X: begin
                int_result = (fp_fmt == FP_FMT_S) ? {{32{s1_raw[31]}}, s1_raw} : d1;
            end

            FP_OP_CLASS: begin
                int_result = (fp_fmt == FP_FMT_S) ? {54'b0, class_s(s1)} : {54'b0, class_d(d1)};
            end

            FP_OP_EQ, FP_OP_LT, FP_OP_LE: begin
                if (fp_fmt == FP_FMT_S) begin
                    if (invalid_s) begin
                        fflags[4] = is_snan_s(s1) || is_snan_s(s2) || (fp_op != FP_OP_EQ);
                        cmp_value = 1'b0;
                    end else begin
                        cmp_value = (fp_op == FP_OP_EQ) ? equal_s(s1, s2) :
                                    (fp_op == FP_OP_LT) ? less_s(s1, s2) :
                                                          (less_s(s1, s2) || equal_s(s1, s2));
                    end
                end else begin
                    if (invalid_d) begin
                        fflags[4] = is_snan_d(d1) || is_snan_d(d2) || (fp_op != FP_OP_EQ);
                        cmp_value = 1'b0;
                    end else begin
                        cmp_value = (fp_op == FP_OP_EQ) ? equal_d(d1, d2) :
                                    (fp_op == FP_OP_LT) ? less_d(d1, d2) :
                                                          (less_d(d1, d2) || equal_d(d1, d2));
                    end
                end
                int_result = {63'b0, cmp_value};
            end

            FP_OP_MIN, FP_OP_MAX: begin
                if (fp_fmt == FP_FMT_S) begin
                    if (is_nan_s(s1) && is_nan_s(s2)) begin
                        fp_result = {32'hFFFF_FFFF, CANONICAL_NAN_S};
                        fflags[4] = is_snan_s(s1) || is_snan_s(s2);
                    end else if (is_nan_s(s1)) begin
                        fp_result = {32'hFFFF_FFFF, s2};
                        fflags[4] = is_snan_s(s1);
                    end else if (is_nan_s(s2)) begin
                        fp_result = {32'hFFFF_FFFF, s1};
                        fflags[4] = is_snan_s(s2);
                    end else if ((s1[30:0] == '0) && (s2[30:0] == '0)) begin
                        fp_result = {32'hFFFF_FFFF, (fp_op == FP_OP_MIN) ? {s1[31] | s2[31], 31'b0} :
                                                                     {s1[31] & s2[31], 31'b0}};
                    end else if (fp_op == FP_OP_MIN) begin
                        fp_result = {32'hFFFF_FFFF, less_s(s1, s2) ? s1 : s2};
                    end else begin
                        fp_result = {32'hFFFF_FFFF, less_s(s1, s2) ? s2 : s1};
                    end
                end else begin
                    if (is_nan_d(d1) && is_nan_d(d2)) begin
                        fp_result = CANONICAL_NAN_D;
                        fflags[4] = is_snan_d(d1) || is_snan_d(d2);
                    end else if (is_nan_d(d1)) begin
                        fp_result = d2;
                        fflags[4] = is_snan_d(d1);
                    end else if (is_nan_d(d2)) begin
                        fp_result = d1;
                        fflags[4] = is_snan_d(d2);
                    end else if ((d1[62:0] == '0) && (d2[62:0] == '0)) begin
                        fp_result = (fp_op == FP_OP_MIN) ? {d1[63] | d2[63], 63'b0} :
                                                           {d1[63] & d2[63], 63'b0};
                    end else if (fp_op == FP_OP_MIN) begin
                        fp_result = less_d(d1, d2) ? d1 : d2;
                    end else begin
                        fp_result = less_d(d1, d2) ? d2 : d1;
                    end
                end
            end

            FP_OP_CVT_F2I: begin
                if (fp_fmt == FP_FMT_S) begin
                    result_int = cvt_s_to_int(s1, rs2_addr[1:0], rm, result_flags);
                end else begin
                    result_int = cvt_d_to_int(d1, rs2_addr[1:0], rm, result_flags);
                end
                int_result = result_int;
                fflags = result_flags;
            end

            FP_OP_CVT_I2F: begin
                result_d = cvt_int_to_fp(int_rs1_data, rs2_addr[1:0], fp_fmt == FP_FMT_D, rm, result_flags);
                fp_result = result_d;
                fflags = result_flags;
            end

            FP_OP_CVT_F2F: begin
                if (fp_fmt == FP_FMT_D) begin
                    result_d = cvt_s_to_d(s1, result_flags);
                    fp_result = result_d;
                    if (is_nan_s(s1)) begin
                        result_flags[4] = is_snan_s(s1);
                    end
                end else begin
                    result_d = cvt_d_to_s(d1, rm, result_flags);
                    fp_result = result_d;
                    if (is_nan_d(d1)) begin
                        result_flags[4] = is_snan_d(d1);
                    end
                end
                fflags = result_flags;
            end

            default: begin
                fp_result = (fp_fmt == FP_FMT_S) ? {32'hFFFF_FFFF, CANONICAL_NAN_S} : CANONICAL_NAN_D;
                int_result = '0;
                fflags[4] = 1'b1;
            end
        endcase
    end

endmodule
