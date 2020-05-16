package com.android.dx.unpacker;

import com.android.dex.Code;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class MethodCodeItem
{
    public int index;
    public String descriptor;
    public int size;
    public byte[] code;

    public Code getCode() {
        ByteBuffer data = ByteBuffer.wrap(code);
        data.order(ByteOrder.LITTLE_ENDIAN);
        int registersSize = data.getShort();
        int insSize = data.getShort();
        int outsSize = data.getShort();
        int triesSize = data.getShort();
        int debugInfoOffset = data.getInt();
        return new Code(registersSize, insSize, outsSize, debugInfoOffset, null, null, null);
    }
}
