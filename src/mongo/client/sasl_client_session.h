/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include <boost/scoped_array.hpp>
#include <sasl/sasl.h>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/client/export_macros.h"

namespace mongo {

    /**
     * Implementation of the client side of a SASL authentication conversation.
     *
     * To use, create an instance, then use setParameter() to configure the authentication
     * parameters.  Once all parameters are set, call initialize() to initialize the client state
     * machine.  Finally, use repeated calls to step() to generate messages to send to the server
     * and process server responses.
     *
     * The required parameters vary by mechanism, but all mechanisms require parameterServiceName,
     * parameterServiceHostname, parameterMechanism and parameterUser.  All of the required
     * parameters must be UTF-8 encoded strings with no embedded NUL characters.  The
     * parameterPassword parameter is not constrained.
     */
    class MONGO_CLIENT_API SaslClientSession {
        MONGO_DISALLOW_COPYING(SaslClientSession);
    public:
        /**
         * Identifiers of parameters used to configure a SaslClientSession.
         */
        enum Parameter {
            parameterServiceName = 0,
            parameterServiceHostname,
            parameterMechanism,
            parameterUser,
            parameterPassword,
            numParameters  // Must be last
        };

        SaslClientSession();
        ~SaslClientSession();

        /**
         * Sets the parameter identified by "id" to "value".
         *
         * The value of "id" must be one of the legal values of Parameter less than numParameters.
         * May be called repeatedly for the same value of "id", with the last "value" replacing
         * previous values.
         *
         * The session object makes and owns a copy of the data in "value".
         */
        void setParameter(Parameter id, const StringData& value);

        /**
         * Returns true if "id" identifies a parameter previously set by a call to setParameter().
         */
        bool hasParameter(Parameter id);

        /**
         * Returns the value of a previously set parameter.
         *
         * If parameter "id" was never set, returns an empty StringData.  Note that a parameter may
         * be explicitly set to StringData(), so use hasParameter() to distinguish those cases.
         *
         * The session object owns the storage behind the returned StringData, which will remain
         * valid until setParameter() is called with the same value of "id", or the session object
         * goes out of scope.
         */
        StringData getParameter(Parameter id);

        /**
         * Returns the value of the parameterPassword parameter in the form of a sasl_secret_t, used
         * by the Cyrus SASL library's SASL_CB_PASS callback.  The session object owns the storage
         * referenced by the returned sasl_secret_t*, which will remain in scope according to the
         * same rules as given for getParameter(), above.
         */
        sasl_secret_t* getPasswordAsSecret();

        /**
         * Initializes a session for use.
         *
         * Call exactly once, after setting any parameters you intend to set via setParameter().
         */
        Status initialize();

        /**
         * Takes one step of the SASL protocol on behalf of the client.
         *
         * Caller should provide data from the server side of the conversation in "inputData", or an
         * empty StringData() if none is available.  If the client should make a response to the
         * server, stores the response into "*outputData".
         *
         * Returns Status::OK() on success.  Any other return value indicates a failed
         * authentication, though the specific return value may provide insight into the cause of
         * the failure (e.g., ProtocolError, AuthenticationFailed).
         *
         * In the event that this method returns Status::OK(), consult the value of isDone() to
         * determine if the conversation has completed.  When step() returns Status::OK() and
         * isDone() returns true, authentication has completed successfully.
         */
        Status step(const StringData& inputData, std::string* outputData);

        /**
         * Returns true if the authentication completed successfully.
         */
        bool isDone() const { return _done; }

    private:
        /**
         * Buffer object that owns data for a single parameter.
         */
        struct DataBuffer {
            boost::scoped_array<char> data;
            size_t size;
        };

        /// Maximum number of Cyrus SASL callbacks stored in _callbacks.
        static const int maxCallbacks = 4;

        /// Underlying Cyrus SASL library connection object.
        sasl_conn_t* _saslConnection;

        /// Callbacks registered on _saslConnection for providing the Cyrus SASL library with
        /// parameter values, etc.
        sasl_callback_t _callbacks[maxCallbacks];

        /// Buffers for each of the settable parameters.
        DataBuffer _parameters[numParameters];

        /// Number of successfully completed conversation steps.
        int _step;

        /// See isDone().
        bool _done;
    };

}  // namespace mongo
