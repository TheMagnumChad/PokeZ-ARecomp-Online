"""Binary-derived module compatibility; IDs bind bytes and never select policy."""
import hashlib,json,struct
from pathlib import Path

SCHEMA='lumiose-module-compatibility-v2'
NATIVE_SCHEMA_V1='lumiose-native-compatibility-v1'
ABI_REVISION=1
MAGIC=b'LUMIOSE-COMPAT1\0'
FOOTER=struct.Struct('<QI16s')
MAX_JSON=1<<20
RUNTIME_MAGIC=b'LCP2\0\0\0\0'
MAX_CAMERA_QUERIES=64

def canonical_build_id(raw):
    if not isinstance(raw,bytes)or len(raw)!=32:raise ValueError('Exact32-byte native build ID required')
    return raw[:20].hex()if raw[20:]==bytes(12)else raw.hex()

def _hex(value,length,name):
    if not isinstance(value,str) or len(value)!=length or any(c not in '0123456789abcdefABCDEF'for c in value):
        raise ValueError('Invalid compatibility '+name)
    return value.lower()

def _int(value,name,minimum=0):
    if type(value)is not int or not minimum<=value<=0xffffffff:raise ValueError('Invalid compatibility '+name)
    return value

def normalize_hook_pools(rows):
    if not isinstance(rows, list) or len(rows) != 2:
        raise ValueError('Expected two executable hook pools')
    ranges = []
    for row in rows:
        if not isinstance(row, list) or len(row) != 2:
            raise ValueError('Invalid executable hook pool')
        start = _int(row[0], 'hook pool offset')
        size = _int(row[1], 'hook pool size', 4)
        if start % 4 or size % 4:
            raise ValueError('Unaligned executable hook pool')
        ranges.append([start, size])
    ranges.sort()
    if sorted(size for _, size in ranges) != [0x1000, 0x4000]:
        raise ValueError('Unexpected executable hook pool sizes')
    if ranges[0][0] + ranges[0][1] > ranges[1][0]:
        raise ValueError('Overlapping executable hook pools')
    return ranges

def validate_hook_pools(module, ranges):
    lo, size = module.text
    hi = lo + size
    for start, length in ranges:
        if start < lo or start + length > hi or start + length > len(module.image):
            raise ValueError('Hook pool outside executable text')
        if any(module.image[start:start + length]):
            raise ValueError('Hook pool is not empty in the base module')

def normalize_profile(value):
    if not isinstance(value,dict) or value.get('schema')not in(SCHEMA,NATIVE_SCHEMA_V1) or type(value.get('abi_revision'))is not int or value['abi_revision']!=ABI_REVISION:
        raise ValueError('Unsupported module compatibility schema/ABI revision')
    short=value.get('build_id');length=len(short)if isinstance(short,str)else 0
    if length not in(40,64):raise ValueError('Compatibility build_id must contain40or64hex characters')
    identity=_hex(short,length,'build_id');full=_hex(value.get('build_id32',identity if length==64 else identity+'0'*24),64,'build_id32')
    if full[:40]!=identity[:40] or(length==64 and full!=identity):raise ValueError('Conflicting compatibility build identities')
    kind=value.get('module_kind','native'if value['schema']==NATIVE_SCHEMA_V1 else None)
    if kind not in('legacy','native'):raise ValueError('Compatibility module_kind must be legacy or native')
    if value['schema']==NATIVE_SCHEMA_V1 and kind!='native':raise ValueError('V1 profile is native ABI only')
    result=dict(schema=value['schema'],abi_revision=ABI_REVISION,module_kind=kind,
        build_id=canonical_build_id(bytes.fromhex(full)),build_id32=full,sha256=_hex(value.get('sha256'),64,'sha256'))
    if 'executable_hook_pools' in value:
        result['executable_hook_pools'] = normalize_hook_pools(value['executable_hook_pools'])
    cameras=value.get('camera_query_returns')
    if not isinstance(cameras,list)or len(cameras)>MAX_CAMERA_QUERIES or(value['schema']==NATIVE_SCHEMA_V1 and len(cameras)!=3):raise ValueError('Invalid camera query return count')
    result['camera_query_returns']=[_int(x,'camera query return',4)for x in cameras]
    if len(set(result['camera_query_returns']))!=len(cameras) or any(x%4 for x in result['camera_query_returns']):raise ValueError('Camera returns must be distinct instruction offsets')
    tables=value.get('switch_tables');entries=value.get('additional_function_entries');witnesses=value.get('witnesses')
    if not isinstance(tables,list)or not isinstance(entries,list)or not isinstance(witnesses,list)or not witnesses:raise ValueError('Qualified tables/entry union/instruction witnesses required')
    result['switch_tables']=[]
    for row in tables:
        if not isinstance(row,(list,tuple))or len(row)!=4 or row[2]not in('b','B','h','H','i','I'):raise ValueError('Invalid relative switch table')
        result['switch_tables'].append([_int(row[0],'table offset'),_int(row[1],'table count',1),row[2],_int(row[3],'table anchor')])
    result['additional_function_entries']=sorted(set(_int(x,'function entry')for x in entries))
    result['witnesses']=[]
    for row in witnesses:
        if not isinstance(row,(list,tuple))or len(row)!=2 or not isinstance(row[1],str)or not row[1]or len(row[1])%2:raise ValueError('Invalid native instruction witness')
        signature=_hex(row[1],len(row[1]),'instruction witness')
        result['witnesses'].append([_int(row[0],'witness offset'),signature])
    if value['schema']==NATIVE_SCHEMA_V1 and value.get('name_preservation_verified')is not True:raise ValueError('V1 native name attestation required')
    if 'name_preservation_verified'in value and type(value['name_preservation_verified'])is not bool:raise ValueError('Invalid name-preservation attestation')
    result['name_preservation_verified']=value.get('name_preservation_verified',False)
    if 'version'in value:
        version=value['version']
        if isinstance(version,str)and 0<len(version)<=64:result['version']=version
        elif type(version)is int and 0<=version<=0xffffffff:result['version']=version
        elif isinstance(version,list)and len(version)==4 and all(type(x)is int and 0<=x<=255 for x in version):result['version']=version
        else:raise ValueError('Invalid optional compatibility version')
    return result

def load_profile(path):
    raw=Path(path).read_bytes()
    if len(raw)>MAX_JSON:raise ValueError('Compatibility metadata exceeds1MiB')
    return normalize_profile(json.loads(raw.decode('utf-8')))

def _hash_prefix(path,length):
    digest=hashlib.sha256()
    with Path(path).open('rb')as f:
        remaining=length
        while remaining:
            block=f.read(min(1<<20,remaining))
            if not block:raise ValueError('Truncated native update')
            digest.update(block);remaining-=len(block)
    return digest.hexdigest()

def _base_identity(path,profile,base_length):
    with Path(path).open('rb')as f:header=f.read(0x60)
    if base_length<0x100 or len(header)!=0x60 or header[:4]!=b'NSO0':raise ValueError('Invalid base native NSO')
    if profile and header[0x40:0x60].hex()!=profile['build_id32']:raise ValueError('Native compatibility full build ID mismatch')

def read_update_profile(candidate):
    path=Path(candidate);length=path.stat().st_size;profile=None;base_length=length;embedded=False
    with path.open('rb')as f:
        if length>=FOOTER.size:
            f.seek(length-FOOTER.size);base,json_length,magic=FOOTER.unpack(f.read(FOOTER.size))
            if magic==MAGIC:
                if not 0<json_length<=MAX_JSON or base<0x100 or base+json_length+FOOTER.size!=length:raise ValueError('Invalid native compatibility trailer bounds')
                f.seek(base);profile=normalize_profile(json.loads(f.read(json_length).decode('utf-8')));base_length=base;embedded=True
    _base_identity(path,profile,base_length);base_hash=_hash_prefix(path,base_length)
    if profile and base_hash!=profile['sha256']:raise ValueError('Embedded compatibility base NSO hash mismatch')
    download_hash=_hash_prefix(path,length)
    if path.stat().st_size!=length:raise ValueError('Native update changed while reading metadata')
    with path.open('rb')as source:source.seek(0x40);identity=source.read(32)
    return dict(embedded=embedded,profile=profile,base_length=base_length,base_sha256=base_hash,download_length=length,download_sha256=download_hash,build_id=canonical_build_id(identity),build_id32=identity.hex())

def normalize_candidate(candidate,output,profile_path=None):
    source=Path(candidate).resolve();out=Path(output).resolve();info=read_update_profile(source);profile=info['profile']
    if profile_path:
        sidecar=load_profile(profile_path)
        if profile is not None and sidecar!=profile:raise ValueError('Conflicting embedded and sidecar compatibility profiles')
        profile=sidecar
    _base_identity(source,profile,info['base_length'])
    if profile and profile['sha256']!=info['base_sha256']:raise ValueError('Compatibility base NSO hash mismatch')
    if out==source or out.exists():raise ValueError('Fresh separate normalized native output required')
    out.parent.mkdir(parents=True,exist_ok=True)
    with source.open('rb')as src,out.open('xb')as dst:
        remaining=info['base_length']
        while remaining:
            block=src.read(min(1<<20,remaining))
            if not block:raise ValueError('Native update changed during normalization')
            dst.write(block);remaining-=len(block)
    if source.stat().st_size!=info['download_length']or _hash_prefix(source,info['download_length'])!=info['download_sha256']or _hash_prefix(out,info['base_length'])!=info['base_sha256']:raise ValueError('Native update changed during normalization')
    return dict(info,path=str(out),selected_profile=profile)

def _validate_bound_profile(module,path,profile):
    value=normalize_profile(profile);size=Path(path).stat().st_size
    _base_identity(path,value,size)
    module.build_id=canonical_build_id(bytes.fromhex(value['build_id32']))
    if _hash_prefix(path,size)!=value['sha256']or module.build_id!=value['build_id']:raise RuntimeError('Native compatibility module hash/build ID mismatch')
    validate_hook_pools(module, value.get('executable_hook_pools', []))
    lo,extent=module.text;hi=lo+extent
    def instruction(offset):return lo<=offset<hi and offset%4==0
    if not all(instruction(x)for x in value['camera_query_returns']):raise RuntimeError('Native camera return outside executable text')
    entries=set(value['additional_function_entries'])
    if not all(instruction(x)for x in entries):raise RuntimeError('Native function entry outside executable text')
    for offset,count,fmt,base in value['switch_tables']:
        width=struct.calcsize(fmt)
        if offset+count*width>len(module.image)or not instruction(base):raise RuntimeError('Native switch table outside image/text bounds')
        targets={base+x*4 for x in struct.unpack_from('<'+str(count)+fmt,module.image,offset)}
        if not all(instruction(x)for x in targets)or not targets<=entries:raise RuntimeError('Native switch target absent from qualified text entry union')
    import_count=0
    for name in('vmp_recomp_camera_find','vmp_recomp_name_ready'):
        indices=[i for i,sym in enumerate(module.syms)if sym[0]==name and sym[2]==0]
        if not indices:continue
        if len(indices)!=1:raise RuntimeError('Compatibility import ambiguous: '+name)
        import_count+=1
        relocs=[r for r in module.relocs if r[2]==indices[0]and r[1]in(257,1025,1026)]
        if not relocs or any(r[3]!=0 or r[0]<module.rodata[0]or r[0]+8>len(module.image)for r in relocs):raise RuntimeError('Native compatibility import relocation invalid: '+name)
    if import_count not in(0,2):raise RuntimeError('Partial native compatibility ABI import pair')
    if (import_count==2)!=(value['module_kind']=='native'):raise RuntimeError('Declared module kind differs from actual import ABI')
    for offset,expected in value['witnesses']:
        signature=bytes.fromhex(expected)
        if offset+len(signature)>len(module.image)or bytes(module.image[offset:offset+len(signature)])!=signature:raise RuntimeError('Native compatibility call or name semantics changed')
    return value

def discover_candidate(module,path,reference=None):
    """Derive policy from this binary. Build IDs are only exact-byte identity."""
    from binary_compat import discover_profile
    discovered=discover_profile(module,Path(path),reference=reference)
    if not isinstance(discovered,dict):raise RuntimeError('Binary compatibility discovery did not return a profile')
    with Path(path).open('rb')as source:source.seek(0x40);identity=source.read(32)
    profile=dict(discovered,schema=SCHEMA,abi_revision=ABI_REVISION,
        build_id=canonical_build_id(identity),build_id32=identity.hex(),sha256=_hash_prefix(path,Path(path).stat().st_size))
    return _validate_bound_profile(module,path,profile)


def validate_candidate(module,path,profile):
    """Optional metadata must agree with independently derived binary facts."""
    value=_validate_bound_profile(module,path,profile)
    discovered=discover_candidate(module,path)
    if value['module_kind']!=discovered['module_kind']or set(value['camera_query_returns'])!=set(discovered['camera_query_returns']):
        raise RuntimeError('Compatibility metadata differs from binary ABI/camera discovery')
    if not {tuple(x)for x in value['switch_tables']}<={tuple(x)for x in discovered['switch_tables']}:
        raise RuntimeError('Compatibility metadata differs from binary switch-table discovery')
    # Older validated metadata can have a larger seed union or omit tables
    # subsequently recovered by the local scanner. Neither reduces the fresh
    # binary-derived closure. Contradictory supplied tables still fail above.
    value['switch_tables']=discovered['switch_tables']
    value['additional_function_entries']=sorted(set(value['additional_function_entries'])|set(discovered['additional_function_entries']))
    if value['schema']==SCHEMA:
        # ABI imports select the native route; they do not independently attest
        # the native name-preservation implementation.
        value['name_preservation_verified']=discovered['name_preservation_verified']
    return value


def runtime_profile_bytes(profile):
    value=normalize_profile(profile)
    if value['schema']==NATIVE_SCHEMA_V1:
        return bytes.fromhex(value['build_id32'])+struct.pack('<III',*value['camera_query_returns'])
    returns=value['camera_query_returns'];flags=1 if value['module_kind']=='native'else 0
    return RUNTIME_MAGIC+struct.pack('<II',flags,len(returns))+bytes.fromhex(value['build_id32'])+struct.pack('<'+'I'*len(returns),*returns)
