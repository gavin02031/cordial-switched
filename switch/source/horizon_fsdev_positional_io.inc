// Compiled inside the matching libnx fs_dev.c, where FsFile ownership and
// devoptab identity can be checked without copying private structs into Morrow.
static fsdev_file_t *morrow_fsdev_file(int descriptor, size_t count, off_t offset,
                                     const void *buffer, bool writing)
{
    __handle *handle = __get_handle(descriptor);
    if (!handle) { errno = EBADF; return NULL; }
    if (handle->device >= STD_MAX || !devoptab_list[handle->device] ||
        devoptab_list[handle->device]->read_r != fsdev_read ||
        devoptab_list[handle->device]->write_r != fsdev_write) {
        errno = ESPIPE;
        return NULL;
    }
    fsdev_file_t *file = handle->fileStruct;
    if (!file || (writing ? (file->flags & O_ACCMODE) == O_RDONLY
                         : (file->flags & O_ACCMODE) == O_WRONLY)) {
        errno = EBADF;
        return NULL;
    }
    if (offset < 0 || count > SSIZE_MAX || count > (u64)INT64_MAX - (u64)offset) {
        errno = EINVAL;
        return NULL;
    }
    if (!buffer && count) { errno = EFAULT; return NULL; }
    // Linux pwrite on O_APPEND has non-POSIX append behaviour. Do not claim
    // to provide an atomic append primitive which fsdev does not have.
    if (writing && (file->flags & O_APPEND)) { errno = EOPNOTSUPP; return NULL; }
    return file;
}

ssize_t morrowFsdevPread(int descriptor, void *buffer, size_t count, off_t offset)
{
    fsdev_file_t *file = morrow_fsdev_file(descriptor, count, offset, buffer, false);
    if (!file) return -1;
    size_t done = 0;
    // Guest pages may be unsuitable for FS IPC. A private staging buffer also
    // avoids the legacy safe-read path's mutable file cursor and EOF loop.
    char staging[0x1000];
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > sizeof(staging)) chunk = sizeof(staging);
        u64 received = 0;
        Result rc = fsFileRead(&file->fd, offset + done, staging, chunk,
                               FsReadOption_None, &received);
        if (R_FAILED(rc)) {
            errno = fsdev_translate_error(rc);
            return done ? (ssize_t)done : -1;
        }
        if (received > chunk) { errno = EIO; return done ? (ssize_t)done : -1; }
        memcpy((char *)buffer + done, staging, received);
        done += received;
        if (received < chunk) break;
    }
    return (ssize_t)done;
}

ssize_t morrowFsdevPwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
    fsdev_file_t *file = morrow_fsdev_file(descriptor, count, offset, buffer, true);
    if (!file) return -1;
    size_t done = 0;
    char staging[0x1000];
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > sizeof(staging)) chunk = sizeof(staging);
        memcpy(staging, (const char *)buffer + done, chunk);
        Result rc = fsFileWrite(&file->fd, offset + done, staging, chunk,
                                file->flags & O_SYNC ? FsWriteOption_Flush : FsWriteOption_None);
        if (R_FAILED(rc)) {
            errno = fsdev_translate_error(rc);
            return done ? (ssize_t)done : -1;
        }
        done += chunk;
    }
    return (ssize_t)done;
}
