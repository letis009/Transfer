# Transfer Node.js Application

## Setup Instructions

1. Clone the repository:
   ```bash
   git clone https://github.com/letis009/Transfer.git
   ```

2. Change into the project directory:
   ```bash
   cd Transfer
   ```

3. Install dependencies:
   ```bash
   npm install
   ```

## Usage Instructions

To start the application, run:
```bash
npm start
```

Visit `http://localhost:3000` in your web browser to access the application. Change the port in the script if necessary.

### Application Workflow

This section explains the detailed operation of the app:

### Middleware:
- **body-parser**: Parses incoming request bodies in a middleware before handling them.
- **cookie-parser**: Parses cookies attached to the client’s request for seamless management across all steps.

### Authorization Code Workflow

#### 1. `GET /authorize`
This endpoint handles the first step, which is to request an authorization code from the Oracle OAuth Provider.

**Process:**
- An HTTP GET request is sent to `${HOST}/oidc-provider/v1/oauth2/authorize`.
- Query parameters such as `response_type`, `client_id`, `scope`, `redirect_uri`, etc., are passed as part of the request.
- Cookies received from the provider are saved to `cookieJar` for later use.
- On successful completion, the user is informed that the authorization step is complete.

#### 2. `POST /signin`
This endpoint logs the user in with their credentials.

**Process:**
- The username, password, and organization name are sent as `x-www-form-urlencoded` data in the POST request body.
- Cookies (from Step 1) are sent as part of the request headers to maintain the session state.
- Updated cookies are saved for use in the token step.
- On success, it informs the user that the sign-in step is completed.

#### 3. `POST /token`
This endpoint exchanges the authorization code for an access token.

**Process:**
- The authorization code, along with additional parameters like `grant_type`, `client_id`, `redirect_uri`, and `code_verifier`, are sent in a POST request.
- The updated `cookieJar` is used to maintain the session state.
- On success, the token is returned as a JSON response.
- If the token retrieval fails, robust error logging helps identify the issue.

### Notes on Code Structure
- **State Management:**
  - `cookieJar` is used to manage cookies across multiple requests in the OAuth flow.
- **Error Handling:**
  - Errors are logged in detail to facilitate debugging.
  - Responses indicate whether each step was successful.

### Next Steps
- Confirm app functionality by running end-to-end tests.
- Start designing the Node-RED nodes to encapsulate this functionality.

### Notes
- Ensure Node.js is installed on your machine.
- This application requires a MongoDB database. Please set up the database configuration in the `.env` file.