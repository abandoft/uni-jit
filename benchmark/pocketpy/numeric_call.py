def numeric_kernel(a, b):
    return (a + b) * (a - 3.25) + b * 0.75


def execute_numeric_kernel(kernel, count):
    lhs = 1.25
    rhs = -7.5
    checksum = 0.0
    for _ in range(count):
        checksum = checksum + kernel(lhs, rhs)
        lhs = lhs + 0.125
        rhs = rhs - 0.0625
        if lhs > 4096.0:
            lhs = 1.25
        if rhs < -4096.0:
            rhs = -7.5
    return checksum


unijit_native_source = (
    "def native(a, b): return (a + b) * (a - 3.25) + b * 0.75"
)
