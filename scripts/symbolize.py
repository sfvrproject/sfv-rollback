# Names functions and lines in the mod DLL for crash stack addresses.
#   python symbolize.py <sfv_rollback.dll> <module base hex> <address hex>...
# The base is logged at startup ("module at ..."); the PDB is found through the
# path recorded in the DLL (build\Release\sfv_rollback.pdb).
import ctypes, ctypes.wintypes as wt, sys

dbghelp = ctypes.WinDLL('dbghelp')
process = ctypes.windll.kernel32.GetCurrentProcess()


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 512)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD),
                ('FileName', ctypes.c_char_p), ('Address', ctypes.c_uint64)]


dbghelp.SymSetOptions(0x2 | 0x10)  # undecorated names, load lines
dbghelp.SymInitialize(process, None, False)
dbghelp.SymLoadModuleEx.restype = ctypes.c_uint64
dll, base = sys.argv[1], int(sys.argv[2], 16)
if not dbghelp.SymLoadModuleEx(process, None, dll.encode(), None, ctypes.c_uint64(base), 0, None, 0):
    sys.exit('cannot load symbols for ' + dll)
for a in sys.argv[3:]:
    address = int(a, 16)
    sym = SYMBOL_INFO(SizeOfStruct=ctypes.sizeof(SYMBOL_INFO) - 512, MaxNameLen=511)
    disp = ctypes.c_uint64()
    name = '?'
    if dbghelp.SymFromAddr(process, ctypes.c_uint64(address), ctypes.byref(disp), ctypes.byref(sym)):
        name = f'{sym.Name.decode()}+0x{disp.value:x}'
    line = IMAGEHLP_LINE64(SizeOfStruct=ctypes.sizeof(IMAGEHLP_LINE64))
    d32 = wt.DWORD()
    where = ''
    if dbghelp.SymGetLineFromAddr64(process, ctypes.c_uint64(address), ctypes.byref(d32), ctypes.byref(line)):
        where = f'  {line.FileName.decode()}:{line.LineNumber}'
    print(f'{address:x}: {name}{where}')
