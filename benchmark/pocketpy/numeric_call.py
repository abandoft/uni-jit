def numeric_workload(count):
    lhs = 1.25
    rhs = -7.5
    checksum = 0.0
    for _ in range(count):
        checksum = checksum + (lhs + rhs) * (lhs - 3.25) + rhs * 0.75
        lhs = lhs + 0.125
        rhs = rhs - 0.0625
        if lhs > 4096.0:
            lhs = 1.25
        if rhs < -4096.0:
            rhs = -7.5
    return checksum


unijit_native_source = "def numeric_workload(count):\n    lhs = 1.25\n    rhs = -7.5\n    checksum = 0.0\n    for iteration in range(count):\n        checksum = checksum + (lhs + rhs) * (lhs - 3.25) + rhs * 0.75\n        lhs = lhs + 0.125\n        rhs = rhs - 0.0625\n        if lhs > 4096.0:\n            lhs = 1.25\n        if rhs < -4096.0:\n            rhs = -7.5\n    return checksum\n"
