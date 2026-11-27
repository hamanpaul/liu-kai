#define WIN32_LEAN_AND_MEAN
#include "../tsf/doctor_scope.hpp"
#include <iostream>

#ifdef INITGUID
#error Doctor scope headers must not enable global GUID definitions.
#endif

#ifdef DOCTOR_SCOPE_LINK_SECOND_UNIT
const GUID &doctor_scope_property_from_second_unit() {
    return boshiamy::doctor::kInputScopeProperty;
}
const GUID &doctor_scope_interface_from_second_unit() {
    return __uuidof(ITfInputScope);
}
#else
const GUID &doctor_scope_property_from_second_unit();
const GUID &doctor_scope_interface_from_second_unit();
int main() {
    if (!IsEqualGUID(doctor_scope_property_from_second_unit(),
            boshiamy::doctor::kInputScopeProperty) ||
        !IsEqualGUID(doctor_scope_interface_from_second_unit(),
            __uuidof(ITfInputScope))) return 1;
    std::cout << "Doctor scope GUID isolation across two translation units passed\n";
    return 0;
}
#endif
