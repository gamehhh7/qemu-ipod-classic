"""Persistent sector COW. Original media is always opened 'rb'.

An exclusive process lock prevents two emulator instances from writing one disk.
SQLite WAL/FULL transactions commit each successful ATA command atomically.
This is virtual-media persistence, NOT a USB device or Windows disk bridge.
"""
from collections.abc import MutableMapping
from pathlib import Path
import hashlib,json,msvcrt,os,sqlite3
from qemu_fc1307_backend import FileBackedStore

class SectorMap(MutableMapping):
    def __init__(self,db,capacity):
        self.db=db;self.capacity=capacity
        self.count=db.execute('SELECT count(*) FROM sectors').fetchone()[0]
    def __getitem__(self,lba):
        row=self.db.execute('SELECT data FROM sectors WHERE lba=?',(lba,)).fetchone()
        if row is None:raise KeyError(lba)
        return row[0]
    def __setitem__(self,lba,data):
        if type(lba) is not int or not 0<=lba<self.capacity or len(data)!=512:
            raise ValueError('sector outside virtual disk / wrong length')
        inserted=self.db.execute('INSERT OR IGNORE INTO sectors(lba,data) VALUES(?,?)',(lba,bytes(data))).rowcount
        if inserted:self.count+=1
        else:self.db.execute('UPDATE sectors SET data=? WHERE lba=?',(bytes(data),lba))
    def __delitem__(self,lba):raise TypeError('overlay sector deletion is not a guest disk operation')
    def __contains__(self,lba):
        return type(lba) is int and self.db.execute('SELECT 1 FROM sectors WHERE lba=?',(lba,)).fetchone() is not None
    def __len__(self):return self.count
    def __iter__(self):return iter([x[0] for x in self.db.execute('SELECT lba FROM sectors ORDER BY lba')])

class PersistentStore(FileBackedStore):
    def __init__(self,image_path,sector_size=512,overlay=None):
        self.db=None;self.lock_file=None;self.closed=False
        super().__init__(image_path,sector_size)
        self.base_image_sectors=self.image_sectors;self.virtual_media=None
        try:
            if self.sector_size!=512:raise ValueError('only 512-byte virtual sectors supported')
            config=os.environ.get('CLASSIC7_VIRTUAL_DISK_JSON')
            if config:
                self.virtual_media=json.loads(Path(config).read_text(encoding='utf-8'))
                n=self.virtual_media['virtual_sectors_512'];start=self.virtual_media['zero_from_sector_512']
                if type(n) is not int or type(start) is not int or not 0<start<self.base_image_sectors<=n<=0x0fffffff:
                    raise ValueError('invalid dedicated virtual disk geometry')
                self.image_sectors=n
            overlay=Path(overlay or os.environ['CLASSIC7_OVERLAY']).absolute()
            if overlay.suffix!='.sqlite3' or overlay.resolve()==Path(image_path).resolve():raise ValueError('separate sqlite3 overlay required')
            if self.virtual_media and not overlay.is_file():
                raise FileNotFoundError('Configured virtual disk is missing; refusing to create a replacement: '+str(overlay))
            overlay.parent.mkdir(parents=True,exist_ok=True)
            self.lock_file=open(str(overlay)+'.lock','a+b');self.lock_file.seek(0)
            if self.lock_file.read(1)==b'':self.lock_file.write(b'0');self.lock_file.flush()
            self.lock_file.seek(0);msvcrt.locking(self.lock_file.fileno(),msvcrt.LK_NBLCK,1)
            self.db=sqlite3.connect(overlay,timeout=5,isolation_level=None)
            self.db.execute('PRAGMA journal_mode=WAL');self.db.execute('PRAGMA synchronous=FULL')
            self.db.execute('CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL)')
            self.db.execute('CREATE TABLE IF NOT EXISTS sectors(lba INTEGER PRIMARY KEY CHECK(lba>=0),data BLOB NOT NULL CHECK(length(data)=512))')
            self._file.seek(0);head=self._file.read(512);self._file.seek(max(0,self.image_size-512));tail=self._file.read(512)
            base_identity={'format':1,'path':os.path.normcase(self.image_path),'size':self.image_size,'sector':512,'head_tail_sha256':hashlib.sha256(head+tail).hexdigest()}
            if self.virtual_media:base_identity.update(virtual_sectors_512=self.image_sectors,zero_from_sector_512=self.virtual_media['zero_from_sector_512'])
            identity=json.dumps(base_identity,sort_keys=True)
            saved=self.db.execute("SELECT value FROM metadata WHERE key='base'").fetchone()
            if saved is not None and saved[0]!=identity:raise ValueError('overlay belongs to a different base image')
            self.db.execute("INSERT OR IGNORE INTO metadata(key,value) VALUES('base',?)",(identity,))
            if self.db.execute('SELECT 1 FROM sectors WHERE lba>=? LIMIT 1',(self.image_sectors,)).fetchone():raise ValueError('overlay contains out-of-capacity sectors')
            self._cow=SectorMap(self.db,self.image_sectors);self.overlay=str(overlay)
        except BaseException:
            self.close();raise
    def begin(self):
        self.db.execute('BEGIN IMMEDIATE');self._count_before_transaction=self._cow.count
    def _read_sector(self,lba):
        if type(lba) is not int or lba<0:raise ValueError('invalid virtual sector')
        # One lookup replaces membership + membership + payload queries.
        # No cache can survive rollback or hide another committed sector.
        row=self.db.execute('SELECT data FROM sectors WHERE lba=?',(lba,)).fetchone()
        if row is not None:self._stats['cow_reads']+=1;return row[0]
        if (self.virtual_media and self.virtual_media['zero_from_sector_512']<=lba<self.image_sectors) or lba>=self.base_image_sectors:
            self._stats['sparse_zero_reads']+=1;return bytes(512)
        self._stats['base_reads']+=1;self._file.seek(lba*512);return self._file.read(512)
    def get(self,lba,default=None):
        return self._read_sector(lba) if type(lba) is int and 0<=lba<self.image_sectors else default
    def __getitem__(self,lba):
        if type(lba) is not int or not 0<=lba<self.image_sectors:raise KeyError(lba)
        return self._read_sector(lba)
    def __len__(self):return self.image_sectors
    def commit(self):self.db.execute('COMMIT')
    def rollback(self):
        self.db.execute('ROLLBACK');self._cow.count=self._count_before_transaction
    def to_dict(self):
        d=super().to_dict();d.update(overlay=self.overlay,persistent=True,note='base opened rb; writes in persistent SQLite WAL FULL COW; no Windows USB bridge claim');return d
    def close(self):
        if self.closed:return
        self.closed=True
        if self.db is not None:
            try:self.db.close()
            finally:self.db=None
        if self.lock_file is not None:
            try:self.lock_file.close()
            finally:self.lock_file=None
        if getattr(self,'_file',None):self._file.close()

