"""Bounded local model identity lookup. Catalog rows stay in memory, never ship.

The player-supplied resource catalog maps national species/form and optional
female visual variant to exact resource paths. No species mapping table is
embedded here. Format field layout is independently parsed and checked.
"""
import ctypes
from functools import lru_cache
import mmap
from pathlib import Path
import re
import struct
from game_ui_font import archive_from_image
from game_ui_models import fnv64, index_from_image, read_members

CATALOG_PATH = 'ik_pokemon/catalog/catalog/poke_resource_table.trpmcatalog'
MAX_CATALOG = 16 * 1024 * 1024

class Buffer:
    def __init__(self, data): self.data = data
    def check(self, p, n):
        if not isinstance(p,int) or p < 0 or n < 0 or p+n > len(self.data):
            raise ValueError('Model catalog range is invalid')
    def number(self, p, fmt):
        self.check(p,struct.calcsize(fmt)); return struct.unpack_from(fmt,self.data,p)[0]
    def u32(self,p): return self.number(p,'<I')
    def indirect(self,p):
        q=p+self.u32(p);self.check(q,4);return q
    def table(self,p):
        vt=p-self.number(p,'<i');self.check(vt,4)
        size=self.number(vt,'<H');obj=self.number(vt+2,'<H')
        if size<4 or size%2 or size>132 or obj<4:raise ValueError('Model catalog table is invalid')
        self.check(vt,size);self.check(p,obj);out=[]
        for i in range((size-4)//2):
            off=self.number(vt+4+2*i,'<H')
            if off and not 4<=off<obj:raise ValueError('Model catalog field is invalid')
            out.append(p+off if off else None)
        return out
    def field(self,t,i): return t[i] if i<len(t) else None
    def vector(self,p,width,limit):
        if p is None:raise ValueError('Model catalog vector is missing')
        q=self.indirect(p);n=self.u32(q)
        if n>limit:raise ValueError('Model catalog vector limit exceeded')
        self.check(q+4,n*width);return n,q+4
    def string(self,p,limit=512):
        if p is None:return ''
        n,q=self.vector(p,1,limit)
        return self.data[q:q+n].decode('ascii')
    def scalar(self,t,i,fmt,default=0):
        p=self.field(t,i);return self.number(p,fmt) if p is not None else default


def resource_pack(index,path):
    b=Buffer(index);root=b.table(b.u32(0));n,hashes=b.vector(b.field(root,0),8,2000000)
    nf,infos=b.vector(b.field(root,2),4,2000000);np,names=b.vector(b.field(root,1),4,100000)
    if n!=nf:raise ValueError('Resource index lengths differ')
    wanted=fnv64(path);lo,hi=0,n
    while lo<hi:
        mid=(lo+hi)//2
        if b.number(hashes+8*mid,'<Q')<wanted:lo=mid+1
        else:hi=mid
    if lo==n or b.number(hashes+8*lo,'<Q')!=wanted:raise ValueError('Local model resource catalog is absent')
    if lo+1<n and b.number(hashes+8*(lo+1),'<Q')==wanted:raise ValueError('Ambiguous resource hash')
    info=b.table(b.indirect(infos+4*lo));pi=b.scalar(info,0,'<I')
    if pi>=np:raise ValueError('Resource pack index invalid')
    name=b.string(names+4*pi)
    if not name.startswith('arc/') or not name.endswith('.trpak') or '..' in name or '\\' in name:
        raise ValueError('Resource pack path invalid')
    return name


def parse_catalog(data):
    if len(data)>MAX_CATALOG:raise ValueError('Model catalog too large')
    b=Buffer(data);root=b.table(b.u32(0));n,entries=b.vector(b.field(root,1),4,10000);rows={}
    for i in range(n):
        entry=b.table(b.indirect(entries+4*i));p=b.field(entry,0)
        if p is None:raise ValueError('Model identity missing')
        spec=b.table(b.indirect(p));species=b.scalar(spec,0,'<H');form=b.scalar(spec,1,'<H');gender=b.scalar(spec,2,'<B')
        # Ignore catalog-only placeholders that cannot be requested by our ABI.
        if not 1<=species<=2047 or not 0<=form<=63:continue
        if gender not in (0,1):raise ValueError(f'Unsupported catalog identity: {species}/{form}/{gender}')
        model=b.string(b.field(entry,1));material=b.string(b.field(entry,2));config=b.string(b.field(entry,3))
        match=re.fullmatch(r'pm(?P<model>\d{4})/(?P<base>pm(?P=model)_\d{2}_\d{2})/(?P=base)\.trmdl',model)
        if not match:raise ValueError('Unsupported model resource path')
        stem=model[:-6]
        if material!=stem+'.trmmt' or config!=stem+'.trpokecfg':raise ValueError('Separate material/config selection needs explicit support')
        key=(species,form,gender)
        if key in rows:raise ValueError('Duplicate model catalog identity')
        rows[key]={'model_path':model,'material_path':material,'config_path':config}
    if not rows:raise ValueError('Empty model catalog')
    return rows


def select_entry(rows,species,form,gender):
    if gender not in (0,1,2):raise ValueError('Invalid gender')
    # Catalog gender 0 is the default resource, including female-only and
    # genderless species. A gender-1 row explicitly overrides it for females.
    key=(species,form,1 if gender==1 else 0)
    entry=rows.get(key)
    if entry is None and gender==1:entry=rows.get((species,form,0))
    if entry is None:raise ValueError('Requested identity is absent from local catalog')
    return entry


@lru_cache(maxsize=2)
def _load_cached(source_name,size,mtime,library_name):
    source=Path(source_name);library=ctypes.CDLL(library_name)
    library.ooz_decompress.argtypes=[ctypes.c_char_p,ctypes.c_size_t,ctypes.c_char_p,ctypes.c_size_t]
    with source.open('rb') as stream,mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ) as mm:
        image=memoryview(mm);pack=image if source.name=='data.trpfs' else archive_from_image(image)
        try:
            index=source.with_name('data.trpfd').read_bytes() if source.name=='data.trpfs' else index_from_image(image)
            name=resource_pack(index,CATALOG_PATH);wanted=fnv64(CATALOG_PATH);total=0
            for h,data in read_members(pack,name,library):
                total+=len(data)
                if total>256*1024*1024:raise ValueError('Catalog group decode budget exceeded')
                if h==wanted:return parse_catalog(data)
            raise ValueError('Catalog member not found in indexed pack')
        finally:
            if pack is not image:pack.release()
            image.release()


def resolve_model(source,library,species,form,gender):
    source=Path(source).resolve();st=source.stat()
    rows=_load_cached(str(source),st.st_size,st.st_mtime_ns,str(Path(library).resolve()))
    return select_entry(rows,species,form,gender)
