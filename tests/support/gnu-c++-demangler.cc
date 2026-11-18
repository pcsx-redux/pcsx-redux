/***************************************************************************
 *   Copyright (C) 2025 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include "gtest/gtest.h"
#include "support/gnu-c++-demangler.h"

TEST(GNUDemangler, _Z1fv) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z1fv"), "f()"); }
TEST(GNUDemangler, _Z1fi) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z1fi"), "f(int)"); }
TEST(GNUDemangler, _Z3foo3bar) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z3foo3bar"), "foo(bar)"); }
TEST(GNUDemangler, _Zrm1XS_) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_Zrm1XS_"), "operator%(X, X)"); }
TEST(GNUDemangler, _ZplR1XS0_) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZplR1XS0_"), "operator+(X&, X&)"); }
TEST(GNUDemangler, _Zoo1XS_) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_Zoo1XS_"), "operator||(X, X)"); }
TEST(GNUDemangler, _ZlsRK1XS1_) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZlsRK1XS1_"), "operator<<(X const&, X const&)");
}
TEST(GNUDemangler, _ZN3FooIA4_iE3barE) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN3FooIA4_iE3barE"), "Foo<int[4]>::bar");
}
TEST(GNUDemangler, _Z1fIiEvi) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z1fIiEvi"), "void f<int>(int)"); }
TEST(GNUDemangler, _Z5firstI3DuoEvS0_) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z5firstI3DuoEvS0_"), "void first<Duo>(Duo)");
}
TEST(GNUDemangler, _Z5firstI3DuoEvT_) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z5firstI3DuoEvT_"), "void first<Duo>(Duo)");
}
TEST(GNUDemangler, _Z3fooIiPFidEiEvv) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z3fooIiPFidEiEvv"), "void foo<int, int (*)(double), int>()");
}
TEST(GNUDemangler, _ZN1N1fE) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN1N1fE"), "N::f"); }
TEST(GNUDemangler, _ZN6System5Sound4beepEv) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN6System5Sound4beepEv"), "System::Sound::beep()");
}
TEST(GNUDemangler, _ZN5Arena5levelE) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN5Arena5levelE"), "Arena::level"); }
TEST(GNUDemangler, _ZN5StackIiiE5levelE) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN5StackIiiE5levelE"), "Stack<int, int>::level");
}
TEST(GNUDemangler, _Z1fI1XEvPVN1AIT_E1TE) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z1fI1XEvPVN1AIT_E1TE"), "void f<X>(A<X>::T volatile*)");
}
TEST(GNUDemangler, _ZngILi42EEvN1AIXplT_Li2EEE1TE) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZngILi42EEvN1AIXplT_Li2EEE1TE"), "void operator-<42>(A<(42)+(2)>::T)");
}
TEST(GNUDemangler, _Z4makeI7FactoryiET_IT0_Ev) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z4makeI7FactoryiET_IT0_Ev"), "Factory<int> make<Factory, int>()");
}
TEST(GNUDemangler, _Z3foo5Hello5WorldS0_S_) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z3foo5Hello5WorldS0_S_"), "foo(Hello, World, World, Hello)");
}
TEST(GNUDemangler, _Z3fooPM2ABi) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z3fooPM2ABi"), "foo(int AB::**)"); }
TEST(GNUDemangler, _ZlsRSoRKSs) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZlsRSoRKSs"), "operator<<(std::ostream&, std::string const&)");
}
TEST(GNUDemangler, _ZTI7a_class) { EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZTI7a_class"), "typeinfo for a_class"); }
TEST(GNUDemangler, _ZN4PCSX10SystemImplC2ERKNS_9ArgumentsE) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN4PCSX10SystemImplC2ERKNS_9ArgumentsE"),
              "PCSX::SystemImpl::SystemImpl(PCSX::Arguments const&)");
}

// Real-world symbols from psyqo torus example
TEST(GNUDemangler, psyqo_sendPrimitive) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN5psyqo3GPU13sendPrimitiveINS_4Prim10VRAMUploadEEEvRKT_"),
              "void psyqo::GPU::sendPrimitive<psyqo::Prim::VRAMUpload>(psyqo::Prim::VRAMUpload const&)");
}
TEST(GNUDemangler, psyqo_ApplicationDtor) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN5psyqo11ApplicationD2Ev"), "psyqo::Application::~Application()");
}
TEST(GNUDemangler, psyqo_iDiv) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN5psyqo19FixedPointInternals4iDivEyjj"),
              "psyqo::FixedPointInternals::iDiv(unsigned long long, unsigned int, unsigned int)");
}
TEST(GNUDemangler, psyqo_generateTable) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN5psyqo13TrigInternals13generateTableERN5eastl5arrayIiLj512EEEj"),
              "psyqo::TrigInternals::generateTable(eastl::array<int, 512>&, unsigned int)");
}
TEST(GNUDemangler, eastl_function_detail) {
    EXPECT_EQ(
        PCSX::GNUDemangler::demangle(
            "_ZN5eastl8internal15function_detailILi8EFvN5psyqo9SimplePad5EventEEE14DefaultInvokerES4_RKNS0_15functor_"
            "storageILi8EEE"),
        "eastl::internal::function_detail<8, void(psyqo::SimplePad::Event)>::DefaultInvoker(psyqo::SimplePad::Event, "
        "eastl::internal::functor_storage<8> const&)");
}
TEST(GNUDemangler, eastl_list_DoErase) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN5eastl4listIN5psyqo3GPU5TimerENS_20fixed_node_"
                                           "allocatorILj40ELj32ELj4ELj0ELb1ENS_9allocatorEEEE7DoEraseEPNS_"
                                           "12ListNodeBaseE"),
              "eastl::list<psyqo::GPU::Timer, eastl::fixed_node_allocator<40, 32, 4, 0, 1, "
              "eastl::allocator>>::DoErase(eastl::ListNodeBase*)");
}
TEST(GNUDemangler, operator_delete) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZdlPvj"), "operator delete(void*, unsigned int)");
}
// Internal linkage qualifier (L before source_name)
TEST(GNUDemangler, internal_linkage) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZN5psyqo6KernelL24fastEnterCriticalSectionEv"),
              "psyqo::Kernel::fastEnterCriticalSection()");
}

// GCC thread-safe static cleanup functions
TEST(GNUDemangler, tcf_cleanup) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("__tcf_ZN12_GLOBAL__N_1L11s_functionsE"),
              "cleanup for _GLOBAL__N_1::s_functions");
}

// Variadic templates (parameter packs)
TEST(GNUDemangler, variadic_func) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z13variadic_funcIJidcEEvDpT_"),
              "void variadic_func<int, double, char>(int, double, char)");
}
TEST(GNUDemangler, variadic_head_tail) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_Z9head_tailIiJdcEEvT_DpT0_"),
              "void head_tail<int, double, char>(int, double, char)");
}

// Lambda symbols
TEST(GNUDemangler, lambda_void) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZZ11uses_lambdavENKUlvE_clEv"),
              "uses_lambda()::{lambda()#1}::operator()() const");
}
TEST(GNUDemangler, lambda_int) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZZ11uses_lambdavENKUliE_clEi"),
              "uses_lambda()::{lambda(int)#1}::operator()(int) const");
}
TEST(GNUDemangler, lambda_int_int) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZZ11uses_lambdavENKUliiE_clEii"),
              "uses_lambda()::{lambda(int, int)#1}::operator()(int, int) const");
}
TEST(GNUDemangler, lambda_capture) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZZ15captured_lambdavENKUlvE_clEv"),
              "captured_lambda()::{lambda()#1}::operator()() const");
}
TEST(GNUDemangler, lambda_in_template) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZZ21templated_with_lambdaIiEvT_ENKUlvE_clEv"),
              "void templated_with_lambda<int>(int)::{lambda()#1}::operator()() const");
}
TEST(GNUDemangler, lambda_in_method) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZZN3Foo6methodEvENKUlvE_clEv"),
              "Foo::method()::{lambda()#1}::operator()() const");
}
TEST(GNUDemangler, lambda_static_fun) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZZN5psyqo6Kernel8Internal7prepareERNS_11ApplicationEENUlvE_4_FUNEv"),
              "psyqo::Kernel::Internal::prepare(psyqo::Application&)::{lambda()#1}::_FUN()");
}

// Vtable and guard variable
TEST(GNUDemangler, vtable) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZTVN5psyqo11ApplicationE"), "vtable for psyqo::Application");
}
TEST(GNUDemangler, guard_variable) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZGVZN12_GLOBAL__N_115getInitializersEvE12initializers"),
              "guard variable for _GLOBAL__N_1::getInitializers()::initializers");
}

// Static local variables and guard variables
TEST(GNUDemangler, static_local_simple) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZZ12simple_guardvE1x"), "simple_guard()::x");
}
TEST(GNUDemangler, static_local_nested) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZZN3Foo12nested_guardEvE1h"), "Foo::nested_guard()::h");
}
TEST(GNUDemangler, static_local_templated) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZZN3Foo15templated_guardIiEET_vE3val"),
              "int Foo::templated_guard<int>()::val");
}
TEST(GNUDemangler, guard_variable_nested) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_ZGVZN3Foo12nested_guardEvE1h"),
              "guard variable for Foo::nested_guard()::h");
}

// Coroutine frame symbols (GCC extension with dot-separated suffixes)
TEST(GNUDemangler, coroutine_actor) {
    EXPECT_EQ(
        PCSX::GNUDemangler::demangle(
            "_ZN12_GLOBAL__N_113CoroutineDemo16generalCoroutineEPZNS0_16generalCoroutineEvE58_ZN12_GLOBAL__N_"
            "113CoroutineDemo16generalCoroutineEv.Frame.actor"),
        "_GLOBAL__N_1::CoroutineDemo::generalCoroutine(_GLOBAL__N_1::CoroutineDemo::generalCoroutine()::_ZN12_GLOBAL__"
        "N_113CoroutineDemo16generalCoroutineEv.Frame*) [actor]");
}
TEST(GNUDemangler, coroutine_destroy) {
    EXPECT_EQ(
        PCSX::GNUDemangler::demangle(
            "_ZN12_GLOBAL__N_113CoroutineDemo16generalCoroutineEPZNS0_16generalCoroutineEvE58_ZN12_GLOBAL__N_"
            "113CoroutineDemo16generalCoroutineEv.Frame.destroy"),
        "_GLOBAL__N_1::CoroutineDemo::generalCoroutine(_GLOBAL__N_1::CoroutineDemo::generalCoroutine()::_ZN12_GLOBAL__"
        "N_113CoroutineDemo16generalCoroutineEv.Frame*) [destroy]");
}

// GCC global constructor/destructor wrappers
TEST(GNUDemangler, global_ctor) {
    EXPECT_EQ(
        PCSX::GNUDemangler::demangle("_GLOBAL__sub_I__ZN5psyqo6Kernel15setBreakHandlerEjON5eastl8functionIFbmEEE"),
        "global constructors keyed to psyqo::Kernel::setBreakHandler(unsigned int, eastl::function<bool(unsigned "
        "long)>&&)");
}
TEST(GNUDemangler, global_ctor_plain) {
    EXPECT_EQ(PCSX::GNUDemangler::demangle("_GLOBAL__sub_I_main"), "global constructors keyed to main");
}

TEST(GNUDemangler, psyqo_sendFragment) {
    EXPECT_EQ(
        PCSX::GNUDemangler::demangle(
            "_ZN5psyqo3GPU12sendFragmentINS_9Fragments25FixedFragmentWithPrologueINS_8FontBase22GlyphsFragmentPrologue"
            "ENS_4Prim6SpriteELj48EEEEEvRKT_ON5eastl8functionIFvvEEENS_3DMA11DmaCallbackE"),
        "void psyqo::GPU::sendFragment<psyqo::Fragments::FixedFragmentWithPrologue<psyqo::FontBase::"
        "GlyphsFragmentPrologue, psyqo::Prim::Sprite, 48>>(psyqo::Fragments::FixedFragmentWithPrologue<psyqo::"
        "FontBase::GlyphsFragmentPrologue, psyqo::Prim::Sprite, 48> const&, eastl::function<void()>&&, psyqo::DMA::"
        "DmaCallback)");
}
