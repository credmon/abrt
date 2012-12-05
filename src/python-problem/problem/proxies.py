import logging

import problem.exception

class DBusProxy(object):
    __instance = None

    def __init__(self, dbus):
        self._proxy = None
        self._iface = None
        self.dbus = dbus
        self.connected = False
        self.connect()

    def __new__(cls, *args, **kwargs):
        if not cls.__instance:
            cls.__instance = super(DBusProxy, cls).__new__(
                                    cls, *args, **kwargs)
        return cls.__instance

    def connect(self):
        self.connected = False
        if self._proxy:
            self._proxy.close()
        try:
            self._proxy = self.dbus.SystemBus().get_object(
                'org.freedesktop.problems', '/org/freedesktop/problems')
        except self.dbus.exceptions.DBusException as e:
            logging.debug('Unable to get dbus proxy: {0}'.format(e.message))
            return

        try:
            self._iface = self.dbus.Interface(self._proxy,
                'org.freedesktop.problems')
        except self.dbus.exceptions.DBusException as e:
            logging.debug('Unable to get dbus interface: {0}'.format(e.message))
            return

        self.connected = True

    def _dbus_call(self, fun_name, *args):
        try:
            return getattr(self._iface, fun_name)(*args)
        except self.dbus.exceptions.DBusException as e:
            dbname = e.get_dbus_name()
            if dbname == "org.freedesktop.DBus.Error.ServiceUnknown":
                self.connect()
                return getattr(self._iface, fun_name)(*args)

            if dbname == 'org.freedesktop.problems.AuthFailure':
                raise problem.exception.AuthFailure(e.message)

            if dbname == 'org.freedesktop.problems.InvalidProblemDir':
                raise problem.exception.InvalidProblem(e.message)

            raise

    def get_item(self, dump_dir, name):
        val = self._dbus_call('GetInfo', dump_dir, [name])
        if name not in val:
            return None

        return str(val[name])

    def set_item(self, dump_dir, name, value):
        return self._dbus_call('SetElement', dump_dir, name, value)

    def del_item(self, dump_dir, name):
        return self._dbus_call('DeleteElement', dump_dir, name)

    def create(self, problem_dict):
        return self._dbus_call('NewProblem', problem_dict)

    def delete(self, dump_dir):
        return self._dbus_call('DeleteProblem', dump_dir)

    def list(self):
        return map(str, self._dbus_call('GetProblems'))

    def list_all(self):
        return map(str, self._dbus_call('GetAllProblems'))

class SocketProxy(object):
    def create(self, problem_dict):
        import socket
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(5)
        try:
            sock.connect('/var/run/abrt/abrt.socket')
            sock.sendall("PUT / HTTP/1.1\r\n\r\n")
            for key, value in problem_dict.iteritems():
                sock.sendall('{0}={1}\0'.format(key.upper(), value))

            sock.shutdown(socket.SHUT_WR)
            resp = ''
            while True:
                buf = sock.recv(256)
                if not buf:
                    break
                resp += buf
            return resp
        except socket.timeout as exc:
            logging.error('communication with daemon failed: {0}'.format(exc))
            return None

    def get_item(self, *args):
        raise NotImplementedError
    def set_item(self, *args):
        raise NotImplementedError
    def del_item(self, *args):
        raise NotImplementedError
    def delete(self, *args):
        raise NotImplementedError
    def list(self, *args):
        raise NotImplementedError

def get_proxy():
    try:
        import dbus
        wrapper = DBusProxy(dbus)
        if wrapper.connected:
            return wrapper
    except ImportError:
        logging.debug('DBus not found')

    return SocketProxy()