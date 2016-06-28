static int32_t s1516_add(int32_t a, int32_t b)
{
    int32_t result;
    result = a + b;
    return result;
}

static int32_t s1516_add_sat(int32_t a, int32_t b)
{
    int32_t result;
    int64_t tmp;

    tmp = (int64_t)a + (int64_t)b;
    if (tmp > (int64_t)0x7FFFFFFF)
        tmp = (int64_t)0x7FFFFFFF;
    if (tmp < -(int64_t)0x80000000)
        tmp = -(int64_t)0x80000000;
    result = (int32_t)tmp;

    return result;
}

// saturate to range of int32_t
static int32_t s1516_sat(int64_t x)
{
    if (x >(int64_t)0x7FFFFFFF) return (int64_t)0x7FFFFFFF;
    else if (x < -(int64_t)0x80000000) return -(int64_t)0x80000000;
    else return (int32_t)x;
}

static int32_t s1516_mul(int32_t a, int32_t b)
{
    int32_t result;
    int64_t temp;

    temp = (int64_t)a * (int64_t)b;
    // Rounding: mid values are rounded up
    temp += 1 << 15;
    // Correct by dividing by base and saturate result
    result = s1516_sat(temp >> 16);

    return result;
}

static int32_t s1516_div(int32_t a, int32_t b)
{
    int32_t result;
    int64_t temp;

    // pre-multiply by the base
    temp = (int64_t)a << 16;
    // Rounding: mid values are rounded up (down for negative values)
    if ((temp >= 0 && b >= 0) || (temp < 0 && b < 0))
        temp += b / 2;
    else
        temp -= b / 2;
    result = (int32_t)(temp / b);

    return result;
}

static int32_t s1516_fma(int32_t a, int32_t b, int32_t c)
{
    int32_t result;
    int64_t temp;

    temp = (int64_t)a * (int64_t)b + ((int64_t)c << 16);

    // Rounding: mid values are rounded up
    temp += 1 << 15;

    // Correct by dividing by base and saturate result
    result = s1516_sat(temp >> 16);

    return result;
}

static int32_t s1516_int(int32_t i)
{
    return i << 16;
}

static int32_t s1516_flt(float f)
{
    return (int32_t)(f * 0xffff);
}