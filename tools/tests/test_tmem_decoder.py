#!/usr/bin/env python3
"""Known color/address vectors plus differential tests against RT64 shader code."""
from pathlib import Path
import importlib.util
import json
import random
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('decode_tmem', ROOT / 'tools/textures/decode_tmem.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


def metadata(fmt=0, siz=2, width=2, height=1, line=1, tmem=0, palette=0, tlut='None'):
    return {'width': width, 'height': height, 'tlut': tlut,
            'tile': {'fmt': fmt, 'siz': siz, 'line': line, 'tmem': tmem, 'palette': palette,
                     'cms': 2, 'cmt': 0, 'masks': 3, 'maskt': 4, 'shifts': 1, 'shiftt': 0,
                     'uls': 4, 'ult': 8, 'lrs': 12, 'lrt': 16}}


class DecodeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build = ROOT / 'build/tmem-decoder-tests'
        build.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(dir=build)
        cls.directory = Path(cls.temp.name)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_rgba16_big_endian_alpha_and_odd_row(self):
        data = bytearray(4096)
        data[8:12] = bytes.fromhex('f801003e')  # red opaque, blue transparent
        data[20:24] = bytes.fromhex('07c1ffff')  # odd row word swapped: green, white
        result = decoder.decode_tmem(data, metadata(height=2, tmem=1))
        self.assertEqual(result, bytes([255,0,0,255, 0,0,255,0, 0,255,0,255, 255,255,255,255]))

    def test_rgba32_split_banks_wrap_and_odd_row(self):
        data = bytearray(4096)
        # Start tmem=255 => byte2040, wrapping the second row into bank zero.
        data[2040:2044] = bytes([11,22,55,66])
        data[4088:4092] = bytes([33,44,77,88])
        data[4:8] = bytes([91,92,95,96])
        data[2052:2056] = bytes([93,94,97,98])
        result = decoder.decode_tmem(data, metadata(siz=3, height=2, tmem=255))
        self.assertEqual(result, bytes([11,22,33,44, 55,66,77,88, 91,92,93,94, 95,96,97,98]))

    def test_ci4_palette_bank_and_tlut(self):
        data = bytearray(4096)
        data[0] = 0x21
        data[0xA90:0xA92] = bytes.fromhex('f801')  # palette5, index2
        data[0xA88:0xA8A] = bytes.fromhex('07c0')  # palette5, index1
        self.assertEqual(decoder.decode_tmem(data, metadata(fmt=2,siz=0,palette=5,tlut='RGBA16')),
                         bytes([255,0,0,255, 0,255,0,0]))

    def test_ci8_ia16_tlut_and_active_tlut_overrides_format(self):
        data = bytearray(4096)
        data[0:2] = bytes([3,254])
        data[0x818:0x81A] = bytes([19,128])
        data[0xFF0:0xFF2] = bytes([234,0])
        expected = bytes([19,19,19,128,234,234,234,0])
        self.assertEqual(decoder.decode_tmem(data, metadata(fmt=2,siz=1,tlut='IA16')), expected)
        self.assertEqual(decoder.decode_tmem(data, metadata(fmt=0,siz=1,tlut='IA16')), expected)

    def test_intensity_and_intensity_alpha(self):
        cases = [(3,0,b'\xb4',bytes([182,182,182,255,73,73,73,0])),
                 (3,1,b'\xa3\xf0',bytes([170,170,170,51,255,255,255,0])),
                 (3,2,b'\x35\xa8\x8d\x00',bytes([53,53,53,168,141,141,141,0])),
                 (4,0,b'\xa3',bytes([170,170,170,170,51,51,51,51])),
                 (4,1,b'\x13\x7e',bytes([19,19,19,19,126,126,126,126]))]
        for fmt,siz,prefix,expected in cases:
            with self.subTest(fmt=fmt,siz=siz):
                self.assertEqual(decoder.decode_tmem(prefix+bytes(4096-len(prefix)),metadata(fmt=fmt,siz=siz)),expected)

    def test_hardware_quirks_and_unsupported_yuv(self):
        data = bytes.fromhex('213456789abcdef0') + bytes(4088)
        self.assertEqual(decoder.decode_tmem(data,metadata(fmt=2,siz=0,palette=5)),bytes([82]*4+[81]*4))
        self.assertEqual(decoder.decode_tmem(data,metadata(fmt=4,siz=2)),bytes.fromhex('2134213456785678'))
        self.assertEqual(decoder.decode_tmem(data,metadata(fmt=1,siz=2)),bytes([0,0,0,255]*2))

    def test_invalid_data(self):
        with self.assertRaisesRegex(ValueError,'4096'):
            decoder.decode_tmem(bytes(4),metadata())
        with self.assertRaisesRegex(ValueError,'line=0'):
            decoder.decode_tmem(bytes(4096),metadata(line=0,height=2))
        with self.assertRaisesRegex(ValueError,'LoadTLUT'):
            decoder.decode_tmem(bytes(4096),metadata(tlut=2))
        # A one-row line=0 has no odd-row load and is unambiguous.
        self.assertEqual(decoder.decode_tmem(bytes(4096),metadata(line=0)),bytes(8))

    def test_png_and_inventory_pixel_aliases(self):
        inp=self.directory/'input';out=self.directory/'output';inp.mkdir(exist_ok=True)
        for i,fmt in enumerate([3,4]):
            base=f'{i+1:016x}.v5'
            (inp/f'{base}.tmem').write_bytes(bytes(4096))
            info=metadata(fmt=fmt,siz=1);info['extra_provenance']={'test':'preserved'}
            (inp/f'{base}.tile.json').write_text(json.dumps(info))
        report=decoder.inventory(inp,out)
        self.assertEqual(report['summary']['decoded_sources'],2)
        self.assertEqual(report['summary']['unique_images'],1)
        self.assertEqual(report['images'][0]['hash_aliases'],['0000000000000001','0000000000000002'])
        self.assertEqual(report['textures'][0]['metadata']['extra_provenance'],{'test':'preserved'})
        png=(out/report['images'][0]['png']).read_bytes()
        offset=8;compressed=b''
        while offset<len(png):
            size=struct.unpack_from('>I',png,offset)[0];kind=png[offset+4:offset+8];data=png[offset+8:offset+8+size]
            self.assertEqual(struct.unpack_from('>I',png,offset+8+size)[0],zlib.crc32(kind+data)&0xffffffff)
            if kind==b'IHDR':self.assertEqual(struct.unpack('>IIBBBBB',data),(2,1,8,6,0,0,0))
            if kind==b'IDAT':compressed+=data
            offset+=12+size
        self.assertEqual(zlib.decompress(compressed),b'\0'+bytes(8))

    def test_against_current_rt64_shader(self):
        compiler=shutil.which('clang++') or shutil.which('c++')
        if not compiler:self.skipTest('A C++ compiler is needed for the direct shader oracle')
        # Compile the actual sampler and color-conversion bodies, not a second
        # hand-written decoding algorithm. The adapter supplies scalar HLSL types.
        formats=(ROOT/'lib/RT64/src/shaders/Formats.hlsli').read_text()
        functions=[]
        for name in ['I4ToFloat4','IA4ToFloat4','I8ToFloat4','IA8ToFloat4','RGBA16ToFloat4','IA16ToFloat4','RGBA32ToFloat4']:
            match=re.search(r'float4 '+name+r'\([^)]*\)\s*\{.*?\n\}',formats,re.S)
            self.assertIsNotNone(match,name);functions.append(match.group())
        sampler=(ROOT/'lib/RT64/src/shaders/TextureDecoder.hlsli').read_text()
        sampler=re.sub(r'^\s*#(?:include|pragma).*$', '', sampler, flags=re.M)
        sampler=sampler.replace('and(', 'both(').replace('or(', 'either(')
        adapter=r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
using uint=uint32_t;
struct int2 { int x,y; int2(int a,int b):x(a),y(b){} };
using uint2=int2;
struct float4 { float v[4]; float4(float a):v{a,a,a,a}{} float4(float a,float b,float c,float d):v{a,b,c,d}{} };
float4 select(bool c,float4 a,float4 b){return c?a:b;}
uint select_uint(bool c,uint a,uint b){return c?a:b;}
bool both(bool a,bool b){return a&&b;}
bool either(bool a,bool b){return a||b;}
template<class T>struct Texture1D { const uint8_t *p; uint Load(int2 i) const{return p[i.x];} };
#define G_IM_FMT_RGBA 0
#define G_IM_FMT_YUV 1
#define G_IM_FMT_CI 2
#define G_IM_FMT_IA 3
#define G_IM_FMT_I 4
#define G_IM_SIZ_4b 0
#define G_IM_SIZ_8b 1
#define G_IM_SIZ_16b 2
#define G_IM_SIZ_32b 3
#define G_TT_RGBA16 32768
#define G_TT_IA16 49152
'''
        driver=r'''
int main(){uint p[8];uint8_t data[4096];while(std::cin.read(reinterpret_cast<char*>(p),sizeof(p))){
if(!std::cin.read(reinterpret_cast<char*>(data),sizeof(data)))return 2;
for(uint y=0;y<p[1];++y)for(uint x=0;x<p[0];++x){auto c=sampleTMEM(int2(x,y),p[3],p[2],p[4]*8,p[5]*8,p[7],p[6],Texture1D<uint>{data});
for(float value:c.v){uint8_t b=uint8_t(std::lround(std::clamp(value,0.f,1.f)*255));std::cout.put(char(b));}}}return 0;}
'''
        cpp=self.directory/'shader_oracle.cpp';exe=self.directory/'shader_oracle'
        cpp.write_text(adapter+'\n'.join(functions)+sampler+driver)
        subprocess.run([compiler,'-std=c++17','-O2',str(cpp),'-o',str(exe)],check=True,capture_output=True)
        rng=random.Random(0x64005);cases=[];payload=bytearray()
        # All shader format/size/TLUT branches, with padded rows, nonzero TMEM
        # starts, palette banks, wraparound, and rows spanning multiple words.
        for fmt in range(8):
            for siz in range(4):
                for tlut in ['None','RGBA16','IA16']:
                    for tmem,line,width,height in [(0,2,17,3),(255,1,9,4),(511,3,23,5)]:
                        info=metadata(fmt,siz,width,height,line,tmem,rng.randrange(16),tlut)
                        data=rng.randbytes(4096)
                        cases.append((data,info))
                        payload+=struct.pack('<8I',width,height,fmt,siz,tmem,line,info['tile']['palette'],{'None':0,'RGBA16':32768,'IA16':49152}[tlut])+data
        result=subprocess.run([str(exe)],input=payload,check=True,capture_output=True).stdout
        cursor=0
        for data,info in cases:
            size=info['width']*info['height']*4
            with self.subTest(tile=info):self.assertEqual(decoder.decode_tmem(data,info),result[cursor:cursor+size])
            cursor+=size
        self.assertEqual(cursor,len(result))


if __name__=='__main__':
    unittest.main()
