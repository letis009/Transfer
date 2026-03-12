const express = require('express');
const axios = require('axios');
const bodyParser = require('body-parser');
const cookieParser = require('cookie-parser');

const app = express();

// Middleware
app.use(bodyParser.urlencoded({ extended: true }));
app.use(cookieParser());

// State to manage cookies internally
let cookieJar = {};

// Step 1: Authorization code request
app.get('/authorize', async (req, res) => {
    const endpoint = `${process.env.HOST}/oidc-provider/v1/oauth2/authorize`;
    const params = {
        response_type: 'code',
        client_id: process.env.CLIENT_ID,
        scope: 'openid',
        redirect_uri: 'apiaccount://callback',
        code_challenge: process.env.CODE_CHALLENGE,
        code_challenge_method: 'S256'
    };
    
    try {
        const response = await axios.get(endpoint, { 
            params,
            withCredentials: true // Handles cookies
        });
        
        // Save cookies for later use
        cookieJar = response.headers['set-cookie'];
        
        res.status(200).send("Authorization step complete. Use cookies.");
    } catch (err) {
        console.error("Authorization request failed:", err.response.data);
        res.status(500).send("Authorization Step failed.");
    }
});

// Step 2: Sign-in
app.post('/signin', async (req, res) => {
    const endpoint = `${process.env.HOST}/oidc-provider/v1/oauth2/signin`;
    const data = new URLSearchParams({
        username: req.body.username,
        password: req.body.password,
        orgname: req.body.orgname
    });
    
    try {
        const response = await axios.post(endpoint, data.toString(), {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            withCredentials: true,
            headers: { Cookie: cookieJar } // Send cookies from previous step
        });
        
        // Update cookieJar for Step 3
        cookieJar = response.headers['set-cookie'];
        
        res.status(200).send("Sign-in step completed. Cookies updated.");
    } catch (err) {
        console.error("Sign-in request failed:", err.response.data);
        res.status(500).send("Sign-in Step failed.");
    }
});

// Step 3: Exchange token
app.post('/token', async (req, res) => {
    const endpoint = `${process.env.HOST}/oidc-provider/v1/oauth2/token`;
    const data = new URLSearchParams({
        scope: 'openid',
        grant_type: 'authorization_code',
        client_id: process.env.CLIENT_ID,
        code_verifier: req.body.code_verifier,
        code: req.body.auth_code,
        redirect_uri: 'apiaccount://callback'
    });
    
    try {
        const response = await axios.post(endpoint, data.toString(), {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            withCredentials: true,
            headers: { Cookie: cookieJar } // Send cookies from previous steps
        });
        
        // Successfully retrieved token
        const token = response.data.access_token;

        // Save to global or respond for monitoring use
        res.status(200).json({ token });
    } catch (err) {
        console.error("Token request failed:", err.response.data);
        res.status(500).send("Token Step failed.");
    }
});

// Server options
const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
    console.log(`Server running on http://localhost:${PORT}`);
});
