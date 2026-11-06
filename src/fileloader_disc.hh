#pragma once

#include "fileloader.hh"

// The disc backend in use, for the code that needs more than the FileLoader
// interface: the drive (CD-DA, silencing) and raw sector reads (streaming).
#if defined(LOADER_ARCHIVE)
#include "fileloader_archive.hh"
namespace psxsplash {
using DiscLoader = FileLoaderArchive;
}
#elif defined(LOADER_CDROM)
#include "fileloader_cdrom.hh"
namespace psxsplash {
using DiscLoader = FileLoaderCDRom;
}
#endif
