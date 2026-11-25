"""Inspect/cycle only the Mini's recorded downstream USB port, never its hub."""
import ctypes as c,ctypes.wintypes as w,sys,json
class GUID(c.Structure):
    _fields_=[('Data1',w.DWORD),('Data2',w.WORD),('Data3',w.WORD),('Data4',c.c_ubyte*8)]
class Interface(c.Structure):
    _fields_=[('cbSize',w.DWORD),('guid',GUID),('flags',w.DWORD),('reserved',c.c_size_t)]
setup=c.WinDLL('setupapi',use_last_error=True);kernel=c.WinDLL('kernel32',use_last_error=True)
setup.SetupDiGetClassDevsW.argtypes=[c.POINTER(GUID),w.LPCWSTR,w.HWND,w.DWORD];setup.SetupDiGetClassDevsW.restype=w.HANDLE
setup.SetupDiEnumDeviceInterfaces.argtypes=[w.HANDLE,w.LPVOID,c.POINTER(GUID),w.DWORD,c.POINTER(Interface)];setup.SetupDiEnumDeviceInterfaces.restype=w.BOOL
setup.SetupDiGetDeviceInterfaceDetailW.argtypes=[w.HANDLE,c.POINTER(Interface),w.LPVOID,w.DWORD,c.POINTER(w.DWORD),w.LPVOID];setup.SetupDiGetDeviceInterfaceDetailW.restype=w.BOOL
setup.SetupDiDestroyDeviceInfoList.argtypes=[w.HANDLE]
kernel.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,w.LPVOID,w.DWORD,w.DWORD,w.HANDLE];kernel.CreateFileW.restype=w.HANDLE
kernel.DeviceIoControl.argtypes=[w.HANDLE,w.DWORD,w.LPVOID,w.DWORD,w.LPVOID,w.DWORD,c.POINTER(w.DWORD),w.LPVOID];kernel.DeviceIoControl.restype=w.BOOL
kernel.CloseHandle.argtypes=[w.HANDLE]
guid=GUID(0xf18a0e88,0xc30c,0x11d0,(c.c_ubyte*8)(0x88,0x15,0x00,0xa0,0xc9,0x06,0xbe,0xd8))
devices=setup.SetupDiGetClassDevsW(c.byref(guid),None,None,0x12)
if devices==c.c_void_p(-1).value:raise c.WinError(c.get_last_error())
paths=[]
try:
    i=0
    while True:
        interface=Interface();interface.cbSize=c.sizeof(interface)
        if not setup.SetupDiEnumDeviceInterfaces(devices,None,c.byref(guid),i,c.byref(interface)):
            if c.get_last_error()==259:break
            raise c.WinError(c.get_last_error())
        i+=1;needed=w.DWORD()
        setup.SetupDiGetDeviceInterfaceDetailW(devices,c.byref(interface),None,0,c.byref(needed),None)
        detail=c.create_string_buffer(needed.value);c.cast(detail,c.POINTER(w.DWORD))[0]=8 if c.sizeof(c.c_void_p)==8 else 6
        if not setup.SetupDiGetDeviceInterfaceDetailW(devices,c.byref(interface),detail,needed.value,c.byref(needed),None):raise c.WinError(c.get_last_error())
        path=c.wstring_at(c.addressof(detail)+4)
        if 'vid_2109&pid_2822#msft20000000001#' in path.lower():paths.append(path)
finally:setup.SetupDiDestroyDeviceInfoList(devices)
assert len(paths)==1,f'Expected one recorded Mini parent hub; got {paths}'
handle=kernel.CreateFileW(paths[0],0xc0000000,3,None,3,0,None)
if handle==c.c_void_p(-1).value:raise c.WinError(c.get_last_error())
try:
    buffer=c.create_string_buffer(512);c.cast(buffer,c.POINTER(w.DWORD))[0]=3;returned=w.DWORD()
    if not kernel.DeviceIoControl(handle,0x220448,buffer,len(buffer),buffer,len(buffer),c.byref(returned),None):raise c.WinError(c.get_last_error())
    import struct
    vendor,product=struct.unpack_from('<HH',buffer.raw,12)
    status=struct.unpack_from('<I',buffer.raw,31)[0]
    print(json.dumps({'hub':paths[0],'port':3,'usb_vendor':hex(vendor),'usb_product':hex(product),'connection_status':status,'returned_bytes':returned.value,'connection_header':buffer.raw[:35].hex()},indent=2),flush=True)
    # Empty/disconnected is expected when the RAM backup script disables its
    # gadget. If something else enumerates here, do not reset that device.
    assert status==0 or (vendor,product) in [(0x1f3a,0xefe8),(0x1d6b,0x0104)],'Port contains an unexpected device; refusing cycle'
    if '--cycle' in sys.argv:
        params=(w.DWORD*2)(3,0)
        if not kernel.DeviceIoControl(handle,0x220444,params,c.sizeof(params),params,c.sizeof(params),c.byref(returned),None):raise c.WinError(c.get_last_error())
        print(f'Cycle request completed; USB status {params[1]:#x}',flush=True)
finally:kernel.CloseHandle(handle)
