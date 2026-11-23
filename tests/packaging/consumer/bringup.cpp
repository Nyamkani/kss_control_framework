#include <bringup/bringup.hpp>
#include <cerrno>

int main()
{
    bringup::RegistrationError error;
    const int result = bringup::Bringup::ValidateElements({}, &error);
    return result == -EINVAL && error.field == "elements" ? 0 : 1;
}
