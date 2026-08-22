struct VSIN { float4 pos : POSITION; };
struct VSOUT { float4 pos : POSITION; };

VSOUT main(VSIN v)
{
    VSOUT o;
    o.pos = v.pos;
    return o;
}
