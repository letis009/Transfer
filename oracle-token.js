module.exports = function(RED) {
    function OracleTokenNode(config) {
        RED.nodes.createNode(this, config);
        const node = this;

        node.on('input', async function(msg, send, done) {
            node.status({ fill: 'blue', shape: 'dot', text: 'Starting token process...' });

            const creds = msg.credentials;

            if (!creds || !creds.endpoint || !creds.username || !creds.password || !creds.client_id) {
                node.status({ fill: 'red', shape: 'ring', text: 'Missing credentials' });
                node.error('Missing required credentials (endpoint, username, password, client_id)');
                done();
                return;
            }

            const axios = require('axios');
            try {
                // Step 1: Get Authorization Code
                node.status({ fill: 'blue', shape: 'dot', text: 'Requesting authorization code...' });
                const authResponse = await axios.get(`${creds.endpoint}/oidc-provider/v1/oauth2/authorize`, {
                    params: {
                        response_type: 'code',
                        client_id: creds.client_id,
                        scope: 'openid',
                        redirect_uri: creds.redirect_uri || 'http://localhost/callback',
                        code_challenge: creds.code_challenge,
                        code_challenge_method: 'S256'
                    },
                    withCredentials: true
                });

                const cookies = authResponse.headers['set-cookie'];

                // Step 2: Perform Sign-in
                node.status({ fill: 'blue', shape: 'dot', text: 'Performing sign-in...' });
                const signInResponse = await axios.post(`${creds.endpoint}/oidc-provider/v1/oauth2/signin`,
                    new URLSearchParams({
                        username: creds.username,
                        password: creds.password,
                        orgname: creds.orgname
                    }).toString(), {
                        headers: {
                            'Content-Type': 'application/x-www-form-urlencoded',
                            Cookie: cookies
                        },
                        withCredentials: true
                    });
                
                const updatedCookies = signInResponse.headers['set-cookie'];

                // Step 3: Get Token
                node.status({ fill: 'blue', shape: 'dot', text: 'Requesting token...' });
                const tokenResponse = await axios.post(`${creds.endpoint}/oidc-provider/v1/oauth2/token`,
                    new URLSearchParams({
                        scope: 'openid',
                        grant_type: 'authorization_code',
                        client_id: creds.client_id,
                        code_verifier: creds.code_verifier,
                        code: creds.auth_code,
                        redirect_uri: creds.redirect_uri || 'http://localhost/callback'
                    }).toString(), {
                        headers: {
                            'Content-Type': 'application/x-www-form-urlencoded',
                            Cookie: updatedCookies
                        },
                        withCredentials: true
                    });
                
                const token = tokenResponse.data.access_token;

                // Output results
                msg.token = token;
                msg.payload = 'Token successfully retrieved';
                global.set('token', token);

                node.status({ fill: 'green', shape: 'dot', text: 'Token retrieved successfully' });
                send(msg);
                done();
            } catch (error) {
                node.status({ fill: 'red', shape: 'ring', text: 'Error retrieving token' });
                node.error(`Error during token process: ${error.message}`);
                done();
            }
        });
    }

    RED.nodes.registerType('oracle-token', OracleTokenNode);
};