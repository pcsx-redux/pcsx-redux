-- lualoader, R"EOF(--
-- MIT License
--
-- Copyright (c) 2023 PCSX-Redux authors
--
-- Permission is hereby granted, free of charge, to any person obtaining a copy
-- of this software and associated documentation files (the "Software"), to deal
-- in the Software without restriction, including without limitation the rights
-- to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
-- copies of the Software, and to permit persons to whom the Software is
-- furnished to do so, subject to the following conditions:
--
-- The above copyright notice and this permission notice shall be included in all
-- copies or substantial portions of the Software.
--
-- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
-- IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
-- FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
-- AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
-- LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
-- OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
-- SOFTWARE.
local checkBImm16 = PCSX.Assembler.Internals.checks.bimm16
local checkGPR = PCSX.Assembler.Internals.checks.gpr
local checkImm5 = PCSX.Assembler.Internals.checks.imm5
local checkImm20 = PCSX.Assembler.Internals.checks.imm20
local checkCOP0 = PCSX.Assembler.Internals.checks.cop0

PCSX.Assembler.Internals.specialInstructions = {
    sll = function(args)
        if #args < 2 or #args > 3 then error('sll takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000000, rd = checkGPR(args[1]), rt = checkGPR(args[1]), imm5 = checkImm5(args[2], 2) }
        end
        return { base = 0x00000000, rd = checkGPR(args[1]), rt = checkGPR(args[2]), imm5 = checkImm5(args[3], 3) }
    end,

    srl = function(args)
        if #args < 2 or #args > 3 then error('srl takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000002, rd = checkGPR(args[1]), rt = checkGPR(args[1]), imm5 = checkImm5(args[2], 2) }
        end
        return { base = 0x00000002, rd = checkGPR(args[1]), rt = checkGPR(args[2]), imm5 = checkImm5(args[3], 3) }
    end,

    sra = function(args)
        if #args < 2 or #args > 3 then error('sra takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000003, rd = checkGPR(args[1]), rt = checkGPR(args[1]), imm5 = checkImm5(args[2], 2) }
        end
        return { base = 0x00000003, rd = checkGPR(args[1]), rt = checkGPR(args[2]), imm5 = checkImm5(args[3], 3) }
    end,

    sllv = function(args)
        if #args < 2 or #args > 3 then error('sllv takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000004, rd = checkGPR(args[1]), rt = checkGPR(args[1]), rs = checkGPR(args[2]) }
        end
        return { base = 0x00000004, rd = checkGPR(args[1]), rt = checkGPR(args[2]), rs = checkGPR(args[3]) }
    end,

    srlv = function(args)
        if #args < 2 or #args > 3 then error('srlv takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000006, rd = checkGPR(args[1]), rt = checkGPR(args[1]), rs = checkGPR(args[2]) }
        end
        return { base = 0x00000006, rd = checkGPR(args[1]), rt = checkGPR(args[2]), rs = checkGPR(args[3]) }
    end,

    srav = function(args)
        if #args < 2 or #args > 3 then error('srav takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000007, rd = checkGPR(args[1]), rt = checkGPR(args[1]), rs = checkGPR(args[2]) }
        end
        return { base = 0x00000007, rd = checkGPR(args[1]), rt = checkGPR(args[2]), rs = checkGPR(args[3]) }
    end,

    jr = function(args)
        if #args ~= 1 then error('jr takes one argument') end
        return { base = 0x00000008, rs = checkGPR(args[1]) }
    end,

    jalr = function(args)
        if #args < 1 or #args > 2 then error('jalr takes one or two arguments') end
        if #args == 1 then return { base = 0x00000009, rs = checkGPR(args[1]), rd = 31 } end
        return { base = 0x00000009, rs = checkGPR(args[1]), rd = checkGPR(args[2]) }
    end,

    syscall = function(args)
        if #args > 1 then error('syscall takes one or no arguments') end
        if #args == 1 then return { base = 0x0000000c, imm20 = checkImm20(args[1]) } end
        return { base = 0x0000000c }
    end,

    break_ = function(args)
        if #args > 1 then error('break takes one or no arguments') end
        if #args == 1 then return { base = 0x0000000d, imm20 = checkImm20(args[1]) } end
        return { base = 0x0000000d }
    end,

    mfhi = function(args)
        if #args ~= 1 then error('mfhi takes one argument') end
        return { base = 0x00000010, rd = checkGPR(args[1]) }
    end,

    mthi = function(args)
        if #args ~= 1 then error('mthi takes one argument') end
        return { base = 0x00000011, rs = checkGPR(args[1]) }
    end,

    mflo = function(args)
        if #args ~= 1 then error('mflo takes one argument') end
        return { base = 0x00000012, rd = checkGPR(args[1]) }
    end,

    mtlo = function(args)
        if #args ~= 1 then error('mtlo takes one argument') end
        return { base = 0x00000013, rs = checkGPR(args[1]) }
    end,

    mult = function(args)
        if #args ~= 2 then error('mult takes two arguments') end
        return { base = 0x00000018, rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
    end,

    multu = function(args)
        if #args ~= 2 then error('multu takes two arguments') end
        return { base = 0x00000019, rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
    end,

    div = function(args)
        if #args ~= 2 then error('div takes two arguments') end
        return { base = 0x0000001a, rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
    end,

    divu = function(args)
        if #args ~= 2 then error('divu takes two arguments') end
        return { base = 0x0000001b, rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
    end,

    add = function(args)
        if #args ~= 3 then error('add takes three arguments') end
        return { base = 0x00000020, rd = checkGPR(args[1]), rs = checkGPR(args[2]), rt = checkGPR(args[3]) }
    end,

    addu = function(args)
        if #args < 2 or #args > 3 then error('addu takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000021, rd = checkGPR(args[1]), rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
        end
        return { base = 0x00000021, rd = checkGPR(args[1]), rs = checkGPR(args[2]), rt = checkGPR(args[3]) }
    end,

    sub = function(args)
        if #args < 2 or #args > 3 then error('sub takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000022, rd = checkGPR(args[1]), rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
        end
        return { base = 0x00000022, rd = checkGPR(args[1]), rs = checkGPR(args[2]), rt = checkGPR(args[3]) }
    end,

    subu = function(args)
        if #args < 2 or #args > 3 then error('subu takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000023, rd = checkGPR(args[1]), rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
        end
        return { base = 0x00000023, rd = checkGPR(args[1]), rs = checkGPR(args[2]), rt = checkGPR(args[3]) }
    end,

    and_ = function(args)
        if #args < 2 or #args > 3 then error('and takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000024, rd = checkGPR(args[1]), rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
        end
        return { base = 0x00000024, rd = checkGPR(args[1]), rs = checkGPR(args[2]), rt = checkGPR(args[3]) }
    end,

    or_ = function(args)
        if #args < 2 or #args > 3 then error('or takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000025, rd = checkGPR(args[1]), rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
        end
        return { base = 0x00000025, rd = checkGPR(args[1]), rs = checkGPR(args[2]), rt = checkGPR(args[3]) }
    end,

    xor_ = function(args)
        if #args < 2 or #args > 3 then error('xor takes two or three arguments') end
        if #args == 2 then
            return { base = 0x00000026, rd = checkGPR(args[1]), rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
        end
        return { base = 0x00000026, rd = checkGPR(args[1]), rs = checkGPR(args[2]), rt = checkGPR(args[3]) }
    end,

    nor = function(args)
        if #args < 2 or #args > 3 then error('nor takes three arguments') end
        if #args == 2 then
            return { base = 0x00000027, rd = checkGPR(args[1]), rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
        end
        return { base = 0x00000027, rd = checkGPR(args[1]), rs = checkGPR(args[2]), rt = checkGPR(args[3]) }
    end,

    slt = function(args)
        if #args < 2 or #args > 3 then error('slt takes three arguments') end
        if #args == 2 then
            return { base = 0x0000002a, rd = checkGPR(args[1]), rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
        end
        return { base = 0x0000002a, rd = checkGPR(args[1]), rs = checkGPR(args[2]), rt = checkGPR(args[3]) }
    end,

    sltu = function(args)
        if #args < 2 or #args > 3 then error('sltu takes three arguments') end
        if #args == 2 then
            return { base = 0x0000002b, rd = checkGPR(args[1]), rs = checkGPR(args[1]), rt = checkGPR(args[2]) }
        end
        return { base = 0x0000002b, rd = checkGPR(args[1]), rs = checkGPR(args[2]), rt = checkGPR(args[3]) }
    end,
}

local renames = {}
for k in pairs(PCSX.Assembler.Internals.specialInstructions) do if k:sub(-1) == '_' then table.insert(renames, k) end end
for _, k in ipairs(renames) do
    local t = PCSX.Assembler.Internals.specialInstructions
    t[k:sub(1, -2)] = t[k]
    t[k] = nil
end

PCSX.Assembler.Internals.bcondInstructions = {
    bltz = function(args)
        if #args ~= 2 then error('bltz takes two arguments') end
        return { base = 0x04000000, rs = checkGPR(args[1]), bimm16 = checkBImm16(args[2], 2) }
    end,

    bgez = function(args)
        if #args ~= 2 then error('bgez takes two arguments') end
        return { base = 0x04010000, rs = checkGPR(args[1]), bimm16 = checkBImm16(args[2], 2) }
    end,

    bltzal = function(args)
        if #args ~= 2 then error('bltzal takes two arguments') end
        return { base = 0x04100000, rs = checkGPR(args[1]), bimm16 = checkBImm16(args[2], 2) }
    end,

    bgezal = function(args)
        if #args ~= 2 then error('bgezal takes two arguments') end
        return { base = 0x04110000, rs = checkGPR(args[1]), bimm16 = checkBImm16(args[2], 2) }
    end,
}

PCSX.Assembler.Internals.cop0Instructions = {
    mfc0 = function(args)
        if #args ~= 2 then error('mfc0 takes two arguments') end
        return { base = 0x40000000, rt = checkGPR(args[1]), rd = checkCOP0(args[2]) }
    end,

    mtc0 = function(args)
        if #args ~= 2 then error('mtc0 takes two arguments') end
        return { base = 0x40800000, rt = checkGPR(args[1]), rd = checkCOP0(args[2]) }
    end,

    rfe = function(args)
        if #args ~= 0 then error('rfe takes no arguments') end
        return { base = 0x42000010 }
    end,
}

-- )EOF"
